#include "base64.h"

#include <jni.h>
#include <jvmti.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <cstdio>
#include <elf.h>
#include <fstream>
#include <iomanip>
#include <link.h>
#include <mqueue.h>
#include <mutex>
#include <openssl/sha.h>
#include <pthread.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

struct SecurityEventMessage {
	char eventName[256];
	char eventData[1024];
};

struct HeartbeatData {
	std::atomic<uint64_t> parentHeartbeat;
	std::atomic<uint64_t> watchdogHeartbeat;
};

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

static std::unordered_set<std::string> g_knownModules;
static std::mutex g_knownModulesMutex;
static std::unordered_set<std::string> g_seenHashes;
static std::mutex g_seenHashesMutex;

static std::thread g_initializationThread;
static std::thread g_scannerThread;

static int g_shmId = -1;
static HeartbeatData* g_heartbeatData = nullptr;
static std::string g_messageQueueName;
static mqd_t g_messageQueue = static_cast<mqd_t>(-1);
static pid_t g_watchdogPid = -1;

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

	pthread_setname_np(pthread_self(), threadName);
}

JNIEnv* AttachThreadWithName(JavaVM* jvm, const char* threadName, bool* needsDetach = nullptr) {
	if (needsDetach) {
		*needsDetach = false;
	}

	if (jvm == nullptr || threadName == nullptr) {
		return nullptr;
	}

	JNIEnv* env = nullptr;
	const jint envStatus = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
	if (envStatus == JNI_EDETACHED) {
		JavaVMAttachArgs args = {};
		args.version = JNI_VERSION_1_6;
		args.name = const_cast<char*>(threadName);
		args.group = nullptr;

		if (jvm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK || env == nullptr) {
			return nullptr;
		}

		if (needsDetach) {
			*needsDetach = true;
		}
		return env;
	}

	if (envStatus != JNI_OK || env == nullptr) {
		return nullptr;
	}

	jclass threadClass = env->FindClass("java/lang/Thread");
	if (!threadClass) {
		if (env->ExceptionCheck()) {
			env->ExceptionClear();
		}
		return env;
	}

	jmethodID currentThreadMethod = env->GetStaticMethodID(threadClass, "currentThread", "()Ljava/lang/Thread;");
	jmethodID setNameMethod = env->GetMethodID(threadClass, "setName", "(Ljava/lang/String;)V");
	if (!currentThreadMethod || !setNameMethod) {
		if (env->ExceptionCheck()) {
			env->ExceptionClear();
		}
		env->DeleteLocalRef(threadClass);
		return env;
	}

	jobject currentThread = env->CallStaticObjectMethod(threadClass, currentThreadMethod);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
	}

	if (currentThread) {
		jstring javaThreadName = env->NewStringUTF(threadName);
		if (javaThreadName) {
			env->CallVoidMethod(currentThread, setNameMethod, javaThreadName);
			if (env->ExceptionCheck()) {
				env->ExceptionClear();
			}
			env->DeleteLocalRef(javaThreadName);
		}
		env->DeleteLocalRef(currentThread);
	}

	env->DeleteLocalRef(threadClass);
	return env;
}

// Provide an external-visible wrapper around the header-only base64
// encoder so that LTO/link-time optimization cannot drop the symbol
// and we avoid undefined hidden symbol errors.
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

static bool IsElfFile(const std::string& filePath) {
	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) {
		return false;
	}

	char header[SELFMAG];
	file.read(header, sizeof(header));
	return file.good() || file.gcount() == static_cast<std::streamsize>(sizeof(header))
		? std::memcmp(header, ELFMAG, SELFMAG) == 0
		: false;
}

static std::string CalculateSHA512(const std::string& filePath) {
	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) {
		return "[Hash Failed: File Open]";
	}

	SHA512_CTX sha512;
	SHA512_Init(&sha512);

	char buffer[4096];
	while (file.read(buffer, sizeof(buffer))) {
		SHA512_Update(&sha512, buffer, file.gcount());
	}
	if (file.gcount() > 0) {
		SHA512_Update(&sha512, buffer, file.gcount());
	}

	unsigned char hash[SHA512_DIGEST_LENGTH];
	SHA512_Final(hash, &sha512);

	static constexpr char kHexDigits[] = "0123456789abcdef";
	std::string result;
	result.reserve(SHA512_DIGEST_LENGTH * 2);
	for (int index = 0; index < SHA512_DIGEST_LENGTH; ++index) {
		const unsigned char byte = hash[index];
		result.push_back(kHexDigits[byte >> 4]);
		result.push_back(kHexDigits[byte & 0x0F]);
	}

	return result;
}

static std::string GetSignerName() {
	return "[Unsigned, Linux]";
}

static std::string GetImportedModules(const std::string& filePath) {
#if defined(__x86_64__) || defined(__aarch64__)
	using Elf_Ehdr_Local = Elf64_Ehdr;
	using Elf_Phdr_Local = Elf64_Phdr;
	using Elf_Dyn_Local = Elf64_Dyn;
	using Elf_Addr_Local = Elf64_Addr;
	using Elf_Off_Local = Elf64_Off;
#elif defined(__i386__) || defined(__arm__)
	using Elf_Ehdr_Local = Elf32_Ehdr;
	using Elf_Phdr_Local = Elf32_Phdr;
	using Elf_Dyn_Local = Elf32_Dyn;
	using Elf_Addr_Local = Elf32_Addr;
	using Elf_Off_Local = Elf32_Off;
#else
#error "Unsupported architecture for ELF parsing"
#endif

	std::ifstream file(filePath, std::ios::binary);
	if (!file) {
		return "";
	}

	file.seekg(0, std::ios::end);
	if (file.tellg() < static_cast<std::streamoff>(sizeof(Elf_Ehdr_Local))) {
		return "";
	}

	file.seekg(0, std::ios::beg);
	Elf_Ehdr_Local elfHeader;
	file.read(reinterpret_cast<char*>(&elfHeader), sizeof(elfHeader));
	if (file.gcount() != static_cast<std::streamsize>(sizeof(elfHeader)) || std::memcmp(elfHeader.e_ident, ELFMAG, SELFMAG) != 0) {
		return "";
	}

	if (elfHeader.e_phoff == 0 || elfHeader.e_phnum == 0) {
		return "";
	}

	file.seekg(elfHeader.e_phoff, std::ios::beg);
	std::vector<Elf_Phdr_Local> programHeaders(elfHeader.e_phnum);
	file.read(reinterpret_cast<char*>(programHeaders.data()), elfHeader.e_phentsize * elfHeader.e_phnum);

	const Elf_Phdr_Local* dynamicHeader = nullptr;
	for (const auto& header : programHeaders) {
		if (header.p_type == PT_DYNAMIC) {
			dynamicHeader = &header;
			break;
		}
	}
	if (!dynamicHeader) {
		return "";
	}

	file.seekg(dynamicHeader->p_offset, std::ios::beg);
	std::vector<Elf_Dyn_Local> dynamicEntries(dynamicHeader->p_filesz / sizeof(Elf_Dyn_Local));
	file.read(reinterpret_cast<char*>(dynamicEntries.data()), dynamicHeader->p_filesz);

	Elf_Addr_Local stringTableAddress = 0;
	for (const auto& entry : dynamicEntries) {
		if (entry.d_tag == DT_STRTAB) {
			stringTableAddress = entry.d_un.d_ptr;
			break;
		}
	}
	if (stringTableAddress == 0) {
		return "";
	}

	Elf_Off_Local stringTableOffset = 0;
	for (const auto& header : programHeaders) {
		if (header.p_type == PT_LOAD && stringTableAddress >= header.p_vaddr && stringTableAddress < header.p_vaddr + header.p_memsz) {
			stringTableOffset = (stringTableAddress - header.p_vaddr) + header.p_offset;
			break;
		}
	}
	if (stringTableOffset == 0) {
		return "";
	}

	std::stringstream result;
	bool firstModule = true;
	for (const auto& entry : dynamicEntries) {
		if (entry.d_tag != DT_NEEDED) {
			continue;
		}

		file.seekg(stringTableOffset + entry.d_un.d_val, std::ios::beg);
		std::string libraryName;
		std::getline(file, libraryName, '\0');
		if (libraryName.empty()) {
			continue;
		}

		if (!firstModule) {
			result << ',';
		}
		result << libraryName;
		firstModule = false;
	}

	return result.str();
}

static ModuleInfo AnalyzeModule(const std::string& modulePath) {
	ModuleInfo info;
	info.path = modulePath;
	info.hash = CalculateSHA512(modulePath);
	info.signerName = GetSignerName();
	info.importedModules = GetImportedModules(modulePath);
	return info;
}

static std::string EncodeImportsList(const std::string& imports) {
	if (imports.empty()) {
		return "";
	}

	std::stringstream input(imports);
	std::stringstream output;
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
		"moduleLoaded " + Base64Encode(info.path) + " " + info.hash + " " + Base64Encode(info.signerName) + " " + EncodeImportsList(info.importedModules);
	LogToMinecraft(formattedMessage);
}

static void ProcessModulePath(const std::string& modulePath) {
	if (modulePath.empty() || g_fairplayDetected.load() || !IsElfFile(modulePath)) {
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
	info.signerName = GetSignerName();
	info.importedModules = GetImportedModules(modulePath);
	LogModuleToMinecraft(info);
}

std::string GetProcessUID(pid_t pid) {
	std::ifstream statusFile("/proc/" + std::to_string(pid) + "/status");
	if (!statusFile) {
		return "[N/A]";
	}

	std::string line;
	while (std::getline(statusFile, line)) {
		if (line.rfind("Uid:", 0) == 0) {
			std::stringstream stream(line);
			std::string label;
			std::string uid;
			stream >> label >> uid;
			return uid;
		}
	}

	return "[N/A]";
}

std::string GetProcessCmdline(pid_t pid) {
	std::ifstream cmdlineFile("/proc/" + std::to_string(pid) + "/cmdline");
	if (!cmdlineFile) {
		return "[Error: Could not open cmdline]";
	}

	std::stringstream stream;
	char current = '\0';
	while (cmdlineFile.get(current)) {
		stream << (current == '\0' ? ' ' : current);
	}

	std::string result = stream.str();
	if (!result.empty() && result.back() == ' ') {
		result.pop_back();
	}

	return result;
}

void RecordModuleCallback(dl_phdr_info* info, size_t, void* data) {
	if (info && info->dlpi_name && info->dlpi_name[0] != '\0') {
		static_cast<std::vector<std::string>*>(data)->push_back(info->dlpi_name);
	}
}

void ProcessSecurityEventQueue() {
	if (g_messageQueue == static_cast<mqd_t>(-1)) {
		return;
	}

	SecurityEventMessage message = {};
	while (mq_receive(g_messageQueue, reinterpret_cast<char*>(&message), sizeof(message), nullptr) != -1) {
		LogToMinecraft(std::string("securityEvent ") + message.eventName + " " + message.eventData);
	}
}

static void RecordBaselineModules() {
	std::vector<std::string> baselineModules;
	dl_iterate_phdr([](dl_phdr_info* info, size_t size, void* data) -> int {
		(void)size;
		RecordModuleCallback(info, 0, data);
		return 0;
	}, &baselineModules);

	std::lock_guard<std::mutex> lock(g_knownModulesMutex);
	for (const auto& modulePath : baselineModules) {
		g_knownModules.insert(modulePath);
	}

	for (const auto& modulePath : baselineModules) {
		ProcessModulePath(modulePath);
	}
}

void WatchdogMain(pid_t parentPid) {
	const mqd_t childQueue = mq_open(g_messageQueueName.c_str(), O_WRONLY);
	if (childQueue == static_cast<mqd_t>(-1)) {
		_exit(1);
	}

	const std::string statusPath = "/proc/" + std::to_string(parentPid) + "/status";
	pid_t lastKnownTracerPid = 0;

	while (true) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		if (g_heartbeatData && g_heartbeatData->watchdogHeartbeat.load() < g_heartbeatData->parentHeartbeat.load()) {
			g_heartbeatData->watchdogHeartbeat.store(g_heartbeatData->parentHeartbeat.load());
		}

		std::ifstream statusFile(statusPath);
		if (!statusFile) {
			break;
		}

		pid_t currentTracerPid = 0;
		std::string line;
		while (std::getline(statusFile, line)) {
			if (line.rfind("TracerPid:", 0) == 0) {
				currentTracerPid = std::stoi(line.substr(line.find('\t') + 1));
				break;
			}
		}

		if (currentTracerPid != 0 && currentTracerPid != lastKnownTracerPid) {
			lastKnownTracerPid = currentTracerPid;

			SecurityEventMessage message = {};
			std::strncpy(message.eventName, "traceDetected", sizeof(message.eventName) - 1);

			const std::string eventData =
				"pid=" + std::to_string(currentTracerPid) +
				", uid=" + GetProcessUID(currentTracerPid) +
				", cmd=" + GetProcessCmdline(currentTracerPid);
			std::strncpy(message.eventData, eventData.c_str(), sizeof(message.eventData) - 1);

			mq_send(childQueue, reinterpret_cast<const char*>(&message), sizeof(message), 0);
		} else if (currentTracerPid == 0) {
			lastKnownTracerPid = 0;
		}
	}

	mq_close(childQueue);
	_exit(0);
}

bool InitializeSecurityMonitoring() {
	g_shmId = shmget(IPC_PRIVATE, sizeof(HeartbeatData), IPC_CREAT | 0666);
	if (g_shmId < 0) {
		return false;
	}

	g_heartbeatData = reinterpret_cast<HeartbeatData*>(shmat(g_shmId, nullptr, 0));
	if (g_heartbeatData == reinterpret_cast<void*>(-1)) {
		g_heartbeatData = nullptr;
		return false;
	}
	g_heartbeatData->parentHeartbeat = 0;
	g_heartbeatData->watchdogHeartbeat = 0;

	g_messageQueueName = "/mc_security_mq_" + std::to_string(getpid());
	mq_unlink(g_messageQueueName.c_str());

	mq_attr attributes = {};
	attributes.mq_flags = 0;
	attributes.mq_maxmsg = 10;
	attributes.mq_msgsize = sizeof(SecurityEventMessage);
	attributes.mq_curmsgs = 0;
	g_messageQueue = mq_open(g_messageQueueName.c_str(), O_CREAT | O_RDONLY | O_NONBLOCK, 0644, &attributes);
	if (g_messageQueue == static_cast<mqd_t>(-1)) {
		return false;
	}

	g_watchdogPid = fork();
	if (g_watchdogPid == 0) {
		WatchdogMain(getppid());
	}

	if (g_watchdogPid < 0) {
		g_watchdogPid = -1;
		return false;
	}

	return true;
}

void ScannerThreadMain() {
	while (g_scannerThreadShouldRun.load()) {
		for (int step = 0; step < 50 && g_scannerThreadShouldRun.load(); ++step) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!g_scannerThreadShouldRun.load()) {
			break;
		}

		ProcessSecurityEventQueue();

		if (g_heartbeatData) {
			const uint64_t lastWatchdogBeat = g_heartbeatData->watchdogHeartbeat.load();
			g_heartbeatData->parentHeartbeat.fetch_add(1);
			std::this_thread::sleep_for(std::chrono::seconds(1));
			if (g_heartbeatData->watchdogHeartbeat.load() == lastWatchdogBeat) {
				g_scannerThreadShouldRun.store(false);
				break;
			}
		}

		std::vector<std::string> currentModules;
		dl_iterate_phdr([](dl_phdr_info* info, size_t size, void* data) -> int {
			(void)size;
			RecordModuleCallback(info, 0, data);
			return 0;
		}, &currentModules);

		std::vector<std::string> newModules;
		{
			std::lock_guard<std::mutex> lock(g_knownModulesMutex);
			for (const auto& modulePath : currentModules) {
				if (g_knownModules.insert(modulePath).second) {
					newModules.push_back(modulePath);
				}
			}
		}

		for (const auto& modulePath : newModules) {
			ProcessModulePath(modulePath);
		}
	}
}

void InitialScanMain() {
	SetCurrentThreadName("LibLogger");

	JavaVM* jvm = WaitForJavaVM();
	if (!jvm) {
		LogStatusToMinecraft("JVM was not found before initialization stopped");
		return;
	}

	bool needsDetach = false;
	AttachThreadWithName(jvm, "LibLogger", &needsDetach);

	for (int step = 0; step < 50 && g_scannerThreadShouldRun.load(); ++step) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!g_scannerThreadShouldRun.load()) {
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return;
	}

	if (IsFairplayLoaded(jvm)) {
		g_fairplayDetected.store(true);
		g_scannerThreadShouldRun.store(false);
		LogStatusToMinecraft("Skipping LibLogger due to Fairplay detection");
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return;
	}

	LogToMinecraft(std::string("Running LibLogger v") + std::string(kLibLoggerVersion) + " for verification purposes");

	if (!InitializeSecurityMonitoring()) {
		g_scannerThreadShouldRun.store(false);
		LogStatusToMinecraft("Failed to initialize LibLogger");
		if (needsDetach) {
			jvm->DetachCurrentThread();
		}
		return;
	}

	RecordBaselineModules();

	g_scannerThread = std::thread([]() {
		SetCurrentThreadName("LibLogger");
		JavaVM* scannerJvm = GetJavaVM();
		bool scannerNeedsDetach = false;
		AttachThreadWithName(scannerJvm, "LibLogger", &scannerNeedsDetach);
		ScannerThreadMain();
		if (scannerNeedsDetach && scannerJvm != nullptr) {
			scannerJvm->DetachCurrentThread();
		}
	});

	if (needsDetach) {
		jvm->DetachCurrentThread();
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

	if (g_watchdogPid > 0) {
		kill(g_watchdogPid, SIGTERM);
		waitpid(g_watchdogPid, nullptr, 0);
		g_watchdogPid = -1;
	}

	if (g_initializationThread.joinable() && g_initializationThread.get_id() != std::this_thread::get_id()) {
		g_initializationThread.join();
	}
	if (g_scannerThread.joinable() && g_scannerThread.get_id() != std::this_thread::get_id()) {
		g_scannerThread.join();
	}

	if (g_messageQueue != static_cast<mqd_t>(-1)) {
		mq_close(g_messageQueue);
		mq_unlink(g_messageQueueName.c_str());
		g_messageQueue = static_cast<mqd_t>(-1);
	}
	g_messageQueueName.clear();

	if (g_heartbeatData) {
		shmdt(g_heartbeatData);
		g_heartbeatData = nullptr;
	}
	if (g_shmId != -1) {
		shmctl(g_shmId, IPC_RMID, nullptr);
		g_shmId = -1;
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
	g_initializationStarted.store(false);
}

__attribute__((constructor)) void LibLoggerInitialize() {
	EnsureInitialized();
}

__attribute__((destructor)) void LibLoggerShutdown() {
	Cleanup();
}
