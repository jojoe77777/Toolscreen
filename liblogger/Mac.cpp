#include "base64.h"

#include <jni.h>
#include <jvmti.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <CommonCrypto/CommonDigest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <libkern/OSByteOrder.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mutex>
#include <pthread.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

struct ModuleInfo {
	std::string path;
	std::string hash;
	std::string signerName;
	std::string importedModules;
};

using PtrJNI_GetCreatedJavaVMs = jint(JNICALL*)(JavaVM**, jsize, jsize*);

static constexpr std::string_view kFairplayClassSignature = "Lexersolver/mcsrfairplay/natives/NativeCallback;";
#ifndef LIBLOGGER_VERSION_STR
#define LIBLOGGER_VERSION_STR "1.0.2"
#endif
static constexpr std::string_view kLibLoggerVersion = LIBLOGGER_VERSION_STR;

static std::atomic<bool> g_initializationStarted(false);
static std::atomic<bool> g_scannerThreadShouldRun(false);
static std::atomic<bool> g_fairplayDetected(false);
static std::atomic<bool> g_addImageCallbackRegistered(false);

static std::unordered_set<std::string> g_knownModules;
static std::mutex g_knownModulesMutex;
static std::unordered_set<std::string> g_seenHashes;
static std::mutex g_seenHashesMutex;

static std::mutex g_pendingModulesMutex;
static std::condition_variable g_pendingModulesCondition;
static std::deque<std::string> g_pendingModules;

static std::thread g_initializationThread;
static std::thread g_scannerThread;

JavaVM* GetJavaVM() {
	auto getCreatedJavaVMs = reinterpret_cast<PtrJNI_GetCreatedJavaVMs>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
	if (!getCreatedJavaVMs) {
		return nullptr;
	}

	JavaVM* jvm = nullptr;
	jsize vmCount = 0;
	if (getCreatedJavaVMs(&jvm, 1, &vmCount) != JNI_OK || vmCount == 0) {
		return nullptr;
	}

	return jvm;
}

JavaVM* WaitForJavaVM() {
	while (g_scannerThreadShouldRun.load()) {
		if (JavaVM* jvm = GetJavaVM()) {
			return jvm;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	return nullptr;
}

bool IsFairplayLoaded(JavaVM* jvm) {
	if (jvm == nullptr) {
		return false;
	}

	JNIEnv* env = nullptr;
	bool needsDetach = false;
	const jint envStatus = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
	if (envStatus == JNI_EDETACHED) {
		if (jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK || env == nullptr) {
			return false;
		}
		needsDetach = true;
	} else if (envStatus != JNI_OK || env == nullptr) {
		return false;
	}

	jvmtiEnv* jvmti = nullptr;
	if (jvm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_0) != JNI_OK || jvmti == nullptr) {
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return false;
	}

	jint classCount = 0;
	jclass* classes = nullptr;
	if (jvmti->GetLoadedClasses(&classCount, &classes) != JVMTI_ERROR_NONE || classes == nullptr) {
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return false;
	}

	bool found = false;
	for (jint index = 0; index < classCount && !found; ++index) {
		char* signature = nullptr;
		if (jvmti->GetClassSignature(classes[index], &signature, nullptr) == JVMTI_ERROR_NONE && signature != nullptr) {
			found = kFairplayClassSignature == signature;
			jvmti->Deallocate(reinterpret_cast<unsigned char*>(signature));
		}
	}

	jvmti->Deallocate(reinterpret_cast<unsigned char*>(classes));
	if (needsDetach) {
		jvm->DetachCurrentThread();
	}
	return found;
}

void SetCurrentThreadName(const char* threadName) {
	if (!threadName) {
		return;
	}

	pthread_setname_np(threadName);
}

__attribute__((visibility("default")))
std::string macaron_base64_encode_visible(const std::string& input) {
	return macaron::Base64::Encode(input);
}

static std::string Base64Encode(const std::string& input) {
	return macaron_base64_encode_visible(input);
}

void LogToMinecraft(const std::string& message) {
	JavaVM* jvm = GetJavaVM();
	if (jvm == nullptr) {
		return;
	}

	JNIEnv* env = nullptr;
	bool needsDetach = false;
	const jint envStatus = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
	if (envStatus == JNI_EDETACHED) {
		JavaVMAttachArgs args = {};
		args.version = JNI_VERSION_1_6;
		args.name = const_cast<char*>("LibLogger");
		args.group = nullptr;
		if (jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), &args) != JNI_OK || env == nullptr) {
			return;
		}
		needsDetach = true;
	} else if (envStatus != JNI_OK || env == nullptr) {
		return;
	}

	jclass systemClass = env->FindClass("java/lang/System");
	if (!systemClass) {
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return;
	}

	jfieldID outFieldId = env->GetStaticFieldID(systemClass, "out", "Ljava/io/PrintStream;");
	if (!outFieldId) {
		env->DeleteLocalRef(systemClass);
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return;
	}

	jobject printStream = env->GetStaticObjectField(systemClass, outFieldId);
	if (!printStream) {
		env->DeleteLocalRef(systemClass);
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return;
	}

	jclass printStreamClass = env->GetObjectClass(printStream);
	if (!printStreamClass) {
		env->DeleteLocalRef(printStream);
		env->DeleteLocalRef(systemClass);
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return;
	}

	jmethodID printlnMethod = env->GetMethodID(printStreamClass, "println", "(Ljava/lang/String;)V");
	if (!printlnMethod) {
		env->DeleteLocalRef(printStreamClass);
		env->DeleteLocalRef(printStream);
		env->DeleteLocalRef(systemClass);
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return;
	}

	jstring logMessage = env->NewStringUTF(message.c_str());
	if (logMessage) {
		env->CallVoidMethod(printStream, printlnMethod, logMessage);
		env->DeleteLocalRef(logMessage);
	}

	if (env->ExceptionCheck()) {
		env->ExceptionClear();
	}

	env->DeleteLocalRef(printStreamClass);
	env->DeleteLocalRef(printStream);
	env->DeleteLocalRef(systemClass);
	if (needsDetach) {
		jvm->DetachCurrentThread();
	}
}

void LogStatusToMinecraft(const std::string& message) {
	LogToMinecraft(std::string("[LibLogger] ") + message);
}

static uint32_t MaybeSwap32(uint32_t value, bool shouldSwap) {
	return shouldSwap ? OSSwapInt32(value) : value;
}

static uint64_t MaybeSwap64(uint64_t value, bool shouldSwap) {
	return shouldSwap ? OSSwapInt64(value) : value;
}

static bool IsMachOFile(const std::string& filePath) {
	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) {
		return false;
	}

	uint32_t magic = 0;
	file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(magic))) {
		return false;
	}

	return magic == MH_MAGIC || magic == MH_CIGAM || magic == MH_MAGIC_64 || magic == MH_CIGAM_64 ||
		magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
}

static std::string CalculateSHA512(const std::string& filePath) {
	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) {
		return "[Hash Failed: File Open]";
	}

	CC_SHA512_CTX sha512;
	CC_SHA512_Init(&sha512);

	char buffer[4096];
	while (file.read(buffer, sizeof(buffer))) {
		CC_SHA512_Update(&sha512, buffer, static_cast<CC_LONG>(file.gcount()));
	}
	if (file.gcount() > 0) {
		CC_SHA512_Update(&sha512, buffer, static_cast<CC_LONG>(file.gcount()));
	}

	unsigned char hash[CC_SHA512_DIGEST_LENGTH];
	CC_SHA512_Final(hash, &sha512);

	static constexpr char kHexDigits[] = "0123456789abcdef";
	std::string result;
	result.reserve(CC_SHA512_DIGEST_LENGTH * 2);
	for (int index = 0; index < CC_SHA512_DIGEST_LENGTH; ++index) {
		const unsigned char byte = hash[index];
		result.push_back(kHexDigits[byte >> 4]);
		result.push_back(kHexDigits[byte & 0x0F]);
	}

	return result;
}

static std::string GetSignerName(const std::string& filePath) {
	CFStringRef pathRef = CFStringCreateWithCString(kCFAllocatorDefault, filePath.c_str(), kCFStringEncodingUTF8);
	if (!pathRef) {
		return "[Unsigned, Signer Check Failed: Path Creation]";
	}

	CFURLRef urlRef = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, pathRef, kCFURLPOSIXPathStyle, false);
	CFRelease(pathRef);
	if (!urlRef) {
		return "[Unsigned, Signer Check Failed: URL Creation]";
	}

	SecStaticCodeRef codeRef = nullptr;
	OSStatus status = SecStaticCodeCreateWithPath(urlRef, kSecCSDefaultFlags, &codeRef);
	CFRelease(urlRef);
	if (status != errSecSuccess) {
		return status == errSecCSUnsigned ? "[Unsigned]" : "[Unsigned, Signer Check Failed]";
	}

	CFDictionaryRef signingInfo = nullptr;
	status = SecCodeCopySigningInformation(codeRef, kSecCSSigningInformation, &signingInfo);
	CFRelease(codeRef);
	if (status != errSecSuccess || !signingInfo) {
		if (signingInfo) {
			CFRelease(signingInfo);
		}
		return "[Unsigned, No Signature Info]";
	}

	std::string signerName = "[Unsigned, No Signer Name Found]";
	CFArrayRef certificates = static_cast<CFArrayRef>(CFDictionaryGetValue(signingInfo, kSecCodeInfoCertificates));
	if (certificates && CFArrayGetCount(certificates) > 0) {
		const auto* certificate = static_cast<const __SecCertificate*>(CFArrayGetValueAtIndex(certificates, 0));
		CFStringRef commonName = nullptr;
		if (certificate && SecCertificateCopyCommonName(const_cast<SecCertificateRef>(certificate), &commonName) == errSecSuccess && commonName) {
			char buffer[256] = {};
			if (CFStringGetCString(commonName, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
				signerName = buffer;
			}
			CFRelease(commonName);
		}
	}

	CFRelease(signingInfo);
	return signerName;
}

static void ParseMachOSlice(std::ifstream& file, std::streamoff offset, std::set<std::string>& importedModules) {
	file.clear();
	file.seekg(offset);

	uint32_t magic = 0;
	file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(magic))) {
		return;
	}

	const bool is64Bit = magic == MH_MAGIC_64 || magic == MH_CIGAM_64;
	const bool shouldSwap = magic == MH_CIGAM || magic == MH_CIGAM_64;
	if (!(magic == MH_MAGIC || magic == MH_CIGAM || magic == MH_MAGIC_64 || magic == MH_CIGAM_64)) {
		return;
	}

	uint32_t commandCount = 0;
	std::streamoff commandOffset = offset;
	if (is64Bit) {
		mach_header_64 header = {};
		file.clear();
		file.seekg(offset);
		file.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
			return;
		}
		commandCount = MaybeSwap32(header.ncmds, shouldSwap);
		commandOffset += sizeof(header);
	} else {
		mach_header header = {};
		file.clear();
		file.seekg(offset);
		file.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
			return;
		}
		commandCount = MaybeSwap32(header.ncmds, shouldSwap);
		commandOffset += sizeof(header);
	}

	for (uint32_t index = 0; index < commandCount; ++index) {
		load_command command = {};
		file.clear();
		file.seekg(commandOffset);
		file.read(reinterpret_cast<char*>(&command), sizeof(command));
		if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(command))) {
			break;
		}

		const uint32_t commandType = MaybeSwap32(command.cmd, shouldSwap);
		const uint32_t commandSize = MaybeSwap32(command.cmdsize, shouldSwap);
		if (commandSize < sizeof(load_command)) {
			break;
		}

		if (commandType == LC_LOAD_DYLIB || commandType == LC_LOAD_WEAK_DYLIB ||
			commandType == LC_REEXPORT_DYLIB || commandType == LC_LOAD_UPWARD_DYLIB) {
			dylib_command dylibCommand = {};
			file.clear();
			file.seekg(commandOffset);
			file.read(reinterpret_cast<char*>(&dylibCommand), sizeof(dylibCommand));
			if (file && file.gcount() == static_cast<std::streamsize>(sizeof(dylibCommand))) {
				const uint32_t nameOffset = MaybeSwap32(dylibCommand.dylib.name.offset, shouldSwap);
				if (nameOffset < commandSize) {
					std::vector<char> nameBuffer(commandSize - nameOffset, '\0');
					file.clear();
					file.seekg(commandOffset + static_cast<std::streamoff>(nameOffset));
					file.read(nameBuffer.data(), static_cast<std::streamsize>(nameBuffer.size()));

					const size_t nameLength = ::strnlen(nameBuffer.data(), nameBuffer.size());
					if (nameLength > 0) {
						importedModules.emplace(nameBuffer.data(), nameLength);
					}
				}
			}
		}

		commandOffset += commandSize;
	}
}

static std::string GetImportedModules(const std::string& filePath) {
	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) {
		return "";
	}

	uint32_t magic = 0;
	file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(magic))) {
		return "";
	}

	std::set<std::string> importedModules;
	if (magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
		const bool is64BitFat = magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
		const bool shouldSwap = magic == FAT_CIGAM || magic == FAT_CIGAM_64;

		fat_header header = {};
		file.clear();
		file.seekg(0);
		file.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
			return "";
		}

		const uint32_t architectureCount = MaybeSwap32(header.nfat_arch, shouldSwap);
		for (uint32_t index = 0; index < architectureCount; ++index) {
			if (is64BitFat) {
				fat_arch_64 architecture = {};
				file.read(reinterpret_cast<char*>(&architecture), sizeof(architecture));
				if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(architecture))) {
					break;
				}
				ParseMachOSlice(file, static_cast<std::streamoff>(MaybeSwap64(architecture.offset, shouldSwap)), importedModules);
			} else {
				fat_arch architecture = {};
				file.read(reinterpret_cast<char*>(&architecture), sizeof(architecture));
				if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(architecture))) {
					break;
				}
				ParseMachOSlice(file, static_cast<std::streamoff>(MaybeSwap32(architecture.offset, shouldSwap)), importedModules);
			}
		}
	} else {
		ParseMachOSlice(file, 0, importedModules);
	}

	std::ostringstream output;
	bool firstModule = true;
	for (const auto& importedModule : importedModules) {
		if (!firstModule) {
			output << ',';
		}
		output << importedModule;
		firstModule = false;
	}

	return output.str();
}

static std::string EncodeImportsList(const std::string& imports) {
	if (imports.empty()) {
		return "";
	}

	std::stringstream input(imports);
	std::ostringstream output;
	std::string item;
	bool firstItem = true;
	while (std::getline(input, item, ',')) {
		const size_t start = item.find_first_not_of(" \t");
		if (start == std::string::npos) {
			continue;
		}

		const size_t end = item.find_last_not_of(" \t");
		const std::string trimmed = item.substr(start, end - start + 1);
		if (!firstItem) {
			output << ',';
		}
		output << Base64Encode(trimmed);
		firstItem = false;
	}

	return output.str();
}

static void LogModuleToMinecraft(const ModuleInfo& info) {
	if (g_fairplayDetected.load()) {
		return;
	}

	const std::string formattedMessage =
		"moduleLoaded " + Base64Encode(info.path) + " " + info.hash + " " +
		Base64Encode(info.signerName) + " " + EncodeImportsList(info.importedModules);
	LogToMinecraft(formattedMessage);
}

static void ProcessModulePath(const std::string& modulePath) {
	if (modulePath.empty() || g_fairplayDetected.load() || !IsMachOFile(modulePath)) {
		return;
	}

	const std::string hash = CalculateSHA512(modulePath);
	{
		std::lock_guard<std::mutex> lock(g_seenHashesMutex);
		if (!g_seenHashes.insert(hash).second) {
			return;
		}
	}

	ModuleInfo info;
	info.path = modulePath;
	info.hash = hash;
	info.signerName = GetSignerName(modulePath);
	info.importedModules = GetImportedModules(modulePath);
	LogModuleToMinecraft(info);
}

static void EnqueueModulePath(const std::string& modulePath) {
	if (modulePath.empty() || !g_scannerThreadShouldRun.load()) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_pendingModulesMutex);
		g_pendingModules.push_back(modulePath);
	}
	g_pendingModulesCondition.notify_one();
}

static void HandleQueuedModule(const std::string& modulePath) {
	if (modulePath.empty() || g_fairplayDetected.load()) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_knownModulesMutex);
		if (!g_knownModules.insert(modulePath).second) {
			return;
		}
	}

	ProcessModulePath(modulePath);
}

static std::vector<std::string> CollectLoadedModules() {
	std::vector<std::string> modules;
	const uint32_t imageCount = _dyld_image_count();
	modules.reserve(imageCount);

	for (uint32_t index = 0; index < imageCount; ++index) {
		const char* imageName = _dyld_get_image_name(index);
		if (imageName && imageName[0] != '\0') {
			modules.emplace_back(imageName);
		}
	}

	return modules;
}

static void RecordBaselineModules() {
	for (const auto& modulePath : CollectLoadedModules()) {
		EnqueueModulePath(modulePath);
	}
}

static void ScannerThreadMain() {
	SetCurrentThreadName("LibLogger");

	while (g_scannerThreadShouldRun.load()) {
		std::deque<std::string> modulesToProcess;
		{
			std::unique_lock<std::mutex> lock(g_pendingModulesMutex);
			g_pendingModulesCondition.wait(lock, []() {
				return !g_scannerThreadShouldRun.load() || !g_pendingModules.empty();
			});

			if (!g_scannerThreadShouldRun.load() && g_pendingModules.empty()) {
				break;
			}

			modulesToProcess.swap(g_pendingModules);
		}

		for (const auto& modulePath : modulesToProcess) {
			if (!g_scannerThreadShouldRun.load()) {
				break;
			}
			HandleQueuedModule(modulePath);
		}
	}
}

static void ImageLoadCallback(const mach_header* header, intptr_t vmaddrSlide) {
	(void)vmaddrSlide;

	if (!g_scannerThreadShouldRun.load() || g_fairplayDetected.load()) {
		return;
	}

	Dl_info info = {};
	if (header && dladdr(header, &info) != 0 && info.dli_fname != nullptr) {
		EnqueueModulePath(info.dli_fname);
	}
}

void InitialScanMain() {
	SetCurrentThreadName("LibLogger");

	JavaVM* jvm = WaitForJavaVM();
	if (!jvm) {
		LogStatusToMinecraft("JVM was not found before initialization stopped");
		return;
	}

	for (int step = 0; step < 50 && g_scannerThreadShouldRun.load(); ++step) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!g_scannerThreadShouldRun.load()) {
		return;
	}

	if (IsFairplayLoaded(jvm)) {
		g_fairplayDetected.store(true);
		g_scannerThreadShouldRun.store(false);
		LogStatusToMinecraft("Skipping LibLogger due to Fairplay detection");
		g_pendingModulesCondition.notify_all();
		return;
	}

	LogToMinecraft(std::string("Running LibLogger v") + std::string(kLibLoggerVersion) + " for verification purposes");

	g_scannerThread = std::thread(ScannerThreadMain);
	RecordBaselineModules();

	bool expected = false;
	if (g_addImageCallbackRegistered.compare_exchange_strong(expected, true)) {
		_dyld_register_func_for_add_image(ImageLoadCallback);
	}
}

void EnsureInitialized() {
	bool expected = false;
	if (!g_initializationStarted.compare_exchange_strong(expected, true)) {
		return;
	}

	g_scannerThreadShouldRun.store(true);
	g_initializationThread = std::thread(InitialScanMain);
}

void Cleanup() {
	g_scannerThreadShouldRun.store(false);
	g_pendingModulesCondition.notify_all();

	if (g_initializationThread.joinable() && g_initializationThread.get_id() != std::this_thread::get_id()) {
		g_initializationThread.join();
	}
	if (g_scannerThread.joinable() && g_scannerThread.get_id() != std::this_thread::get_id()) {
		g_scannerThread.join();
	}

	{
		std::lock_guard<std::mutex> lock(g_pendingModulesMutex);
		g_pendingModules.clear();
	}
	{
		std::lock_guard<std::mutex> lock(g_knownModulesMutex);
		g_knownModules.clear();
	}
	{
		std::lock_guard<std::mutex> lock(g_seenHashesMutex);
		g_seenHashes.clear();
	}

	g_fairplayDetected.store(false);
	g_addImageCallbackRegistered.store(false);
	g_initializationStarted.store(false);
}

__attribute__((constructor)) void LibLoggerInitialize() {
	EnsureInitialized();
}

__attribute__((destructor)) void LibLoggerShutdown() {
	Cleanup();
}
