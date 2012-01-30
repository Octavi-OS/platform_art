/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "UniquePtr.h"
#include "class_linker.h"
#include "class_loader.h"
#include "compiler.h"
#include "constants.h"
#include "dex_file.h"
#include "file.h"
#include "gtest/gtest.h"
#include "heap.h"
#include "macros.h"
#include "oat_file.h"
#include "object_utils.h"
#include "os.h"
#include "runtime.h"
#include "stl_util.h"
#include "stringprintf.h"
#include "thread.h"
#include "unicode/uclean.h"
#include "unicode/uvernum.h"

namespace art {

static const byte kBase64Map[256] = {
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
  52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255,
  255, 254, 255, 255, 255,   0,   1,   2,   3,   4,   5,   6,
    7,   8,   9,  10,  11,  12,  13,  14,  15,  16,  17,  18,
   19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255, 255,
  255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,
   37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
   49,  50,  51, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255
};

byte* DecodeBase64(const char* src, size_t* dst_size) {
  std::vector<byte> tmp;
  unsigned long t = 0, y = 0;
  int g = 3;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    byte c = kBase64Map[src[i] & 0xFF];
    if (c == 255) continue;
    // the final = symbols are read and used to trim the remaining bytes
    if (c == 254) {
      c = 0;
      // prevent g < 0 which would potentially allow an overflow later
      if (--g < 0) {
        return NULL;
      }
    } else if (g != 3) {
      // we only allow = to be at the end
      return NULL;
    }
    t = (t << 6) | c;
    if (++y == 4) {
      tmp.push_back((t >> 16) & 255);
      if (g > 1) {
        tmp.push_back((t >> 8) & 255);
      }
      if (g > 2) {
        tmp.push_back(t & 255);
      }
      y = t = 0;
    }
  }
  if (y != 0) {
    return NULL;
  }
  UniquePtr<byte[]> dst(new byte[tmp.size()]);
  if (dst_size != NULL) {
    *dst_size = tmp.size();
  }
  std::copy(tmp.begin(), tmp.end(), dst.get());
  return dst.release();
}

static inline const DexFile* OpenDexFileBase64(const char* base64,
                                               const std::string& location) {
  // decode base64
  CHECK(base64 != NULL);
  size_t length;
  UniquePtr<byte[]> dex_bytes(DecodeBase64(base64, &length));
  CHECK(dex_bytes.get() != NULL);

  // write to provided file
  UniquePtr<File> file(OS::OpenFile(location.c_str(), true));
  CHECK(file.get() != NULL);
  if (!file->WriteFully(dex_bytes.get(), length)) {
    PLOG(FATAL) << "Failed to write base64 as dex file";
  }
  file.reset();

  // read dex file
  const DexFile* dex_file = DexFile::Open(location, "");
  CHECK(dex_file != NULL);
  return dex_file;
}

class ScratchFile {
 public:
  ScratchFile() {
    filename_ = getenv("ANDROID_DATA");
    filename_ += "/TmpFile-XXXXXX";
    fd_ = mkstemp(&filename_[0]);
    CHECK_NE(-1, fd_);
    file_.reset(OS::FileFromFd(GetFilename(), fd_));
  }

  ~ScratchFile() {
    int unlink_result = unlink(filename_.c_str());
    CHECK_EQ(0, unlink_result);
    int close_result = close(fd_);
    CHECK_EQ(0, close_result);
  }

  const char* GetFilename() const {
    return filename_.c_str();
  }

  File* GetFile() const {
    return file_.get();
  }

  int GetFd() const {
    return fd_;
  }

 private:
  std::string filename_;
  int fd_;
  UniquePtr<File> file_;
};

class CommonTest : public testing::Test {
 public:

  static void MakeExecutable(const ByteArray* code_array) {
    CHECK(code_array != NULL);
    MakeExecutable(code_array->GetData(), code_array->GetLength());
  }

  static void MakeExecutable(const std::vector<uint8_t>& code) {
    CHECK_NE(code.size(), 0U);
    MakeExecutable(&code[0], code.size());
  }

  // Create an OatMethod based on pointers (for unit tests)
  OatFile::OatMethod CreateOatMethod(const void* code,
                                     const size_t frame_size_in_bytes,
                                     const uint32_t core_spill_mask,
                                     const uint32_t fp_spill_mask,
                                     const uint32_t* mapping_table,
                                     const uint16_t* vmap_table,
                                     const uint8_t* gc_map,
                                     const Method::InvokeStub* invoke_stub) {
      return OatFile::OatMethod(NULL,
                                reinterpret_cast<uint32_t>(code),
                                frame_size_in_bytes,
                                core_spill_mask,
                                fp_spill_mask,
                                reinterpret_cast<uint32_t>(mapping_table),
                                reinterpret_cast<uint32_t>(vmap_table),
                                reinterpret_cast<uint32_t>(gc_map),
                                reinterpret_cast<uint32_t>(invoke_stub));
  }

  void MakeExecutable(Method* method) {
    CHECK(method != NULL);

    MethodHelper mh(method);
    const CompiledInvokeStub* compiled_invoke_stub =
        compiler_->FindInvokeStub(mh.IsStatic(), mh.GetShorty());
    CHECK(compiled_invoke_stub != NULL) << PrettyMethod(method);
    const std::vector<uint8_t>& invoke_stub = compiled_invoke_stub->GetCode();
    MakeExecutable(invoke_stub);
    const Method::InvokeStub* method_invoke_stub
        = reinterpret_cast<const Method::InvokeStub*>(&invoke_stub[0]);
    LOG(INFO) << "MakeExecutable " << PrettyMethod(method)
              << " invoke_stub=" << reinterpret_cast<void*>(method_invoke_stub);

    if (!method->IsAbstract()) {
      const DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
      const DexFile& dex_file = Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache);
      const CompiledMethod* compiled_method =
          compiler_->GetCompiledMethod(Compiler::MethodReference(&dex_file,
                                                                 method->GetDexMethodIndex()));
      CHECK(compiled_method != NULL) << PrettyMethod(method);
      const std::vector<uint8_t>& code = compiled_method->GetCode();
      MakeExecutable(code);
      const void* method_code
          = CompiledMethod::CodePointer(&code[0], compiled_method->GetInstructionSet());
      LOG(INFO) << "MakeExecutable " << PrettyMethod(method) << " code=" << method_code;
      OatFile::OatMethod oat_method = CreateOatMethod(method_code,
                                                      compiled_method->GetFrameSizeInBytes(),
                                                      compiled_method->GetCoreSpillMask(),
                                                      compiled_method->GetFpSpillMask(),
                                                      &compiled_method->GetMappingTable()[0],
                                                      &compiled_method->GetVmapTable()[0],
                                                      NULL,
                                                      method_invoke_stub);
      oat_method.LinkMethodPointers(method);
    } else {
      MakeExecutable(runtime_->GetAbstractMethodErrorStubArray());
      const void* method_code = runtime_->GetAbstractMethodErrorStubArray()->GetData();
      LOG(INFO) << "MakeExecutable " << PrettyMethod(method) << " code=" << method_code;
      OatFile::OatMethod oat_method = CreateOatMethod(method_code,
                                                      kStackAlignment,
                                                      0,
                                                      0,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      method_invoke_stub);
      oat_method.LinkMethodPointers(method);
    }
  }

  static void MakeExecutable(const void* code_start, size_t code_length) {
    CHECK(code_start != NULL);
    CHECK_NE(code_length, 0U);
    uintptr_t data = reinterpret_cast<uintptr_t>(code_start);
    uintptr_t base = RoundDown(data, kPageSize);
    uintptr_t limit = RoundUp(data + code_length, kPageSize);
    uintptr_t len = limit - base;
    int result = mprotect(reinterpret_cast<void*>(base), len, PROT_READ | PROT_WRITE | PROT_EXEC);
    CHECK_EQ(result, 0);

    // Flush instruction cache
    // Only uses __builtin___clear_cache if GCC >= 4.3.3
#if GCC_VERSION >= 40303
    __builtin___clear_cache(reinterpret_cast<void*>(base), reinterpret_cast<void*>(base + len));
#elif defined(__APPLE__)
    // Currently, only Mac OS builds use GCC 4.2.*. Those host builds do not
    // need to generate clear_cache on x86.
#else
#error unsupported
#endif
  }

 protected:
  virtual void SetUp() {
    is_host_ = getenv("ANDROID_BUILD_TOP") != NULL;

    if (is_host_) {
      // $ANDROID_ROOT is set on the device, but not on the host.
      // We need to set this so that icu4c can find its locale data.
      std::string root;
      root += getenv("ANDROID_BUILD_TOP");
#if defined(__linux__)
      root += "/out/host/linux-x86";
#elif defined(__APPLE__)
      root += "/out/host/darwin-x86";
#else
#error unsupported OS
#endif
      setenv("ANDROID_ROOT", root.c_str(), 1);
    }

    // On target, Cannot use /mnt/sdcard because it is mounted noexec, so use subdir of art-cache
    android_data_ = (is_host_ ? "/tmp/art-data-XXXXXX" : "/data/art-cache/art-data-XXXXXX");
    if (mkdtemp(&android_data_[0]) == NULL) {
      PLOG(FATAL) << "mkdtemp(\"" << &android_data_[0] << "\") failed";
    }
    setenv("ANDROID_DATA", android_data_.c_str(), 1);
    art_cache_.append(android_data_.c_str());
    art_cache_.append("/art-cache");
    int mkdir_result = mkdir(art_cache_.c_str(), 0700);
    ASSERT_EQ(mkdir_result, 0);

    java_lang_dex_file_.reset(DexFile::Open(GetLibCoreDexFileName(), ""));

    std::string boot_class_path;
    boot_class_path += "-Xbootclasspath:";
    boot_class_path += GetLibCoreDexFileName();

    std::string min_heap_string(StringPrintf("-Xms%zdm", Heap::kInitialSize / MB));
    std::string max_heap_string(StringPrintf("-Xmx%zdm", Heap::kMaximumSize / MB));

    Runtime::Options options;
    options.push_back(std::make_pair("compiler", reinterpret_cast<void*>(NULL)));
    options.push_back(std::make_pair(boot_class_path.c_str(), reinterpret_cast<void*>(NULL)));
    options.push_back(std::make_pair("-Xcheck:jni", reinterpret_cast<void*>(NULL)));
    options.push_back(std::make_pair(min_heap_string.c_str(), reinterpret_cast<void*>(NULL)));
    options.push_back(std::make_pair(max_heap_string.c_str(), reinterpret_cast<void*>(NULL)));
    runtime_.reset(Runtime::Create(options, false));
    ASSERT_TRUE(runtime_.get() != NULL);
    class_linker_ = runtime_->GetClassLinker();

    InstructionSet instruction_set = kNone;
#if defined(__i386__)
    instruction_set = kX86;
#elif defined(__arm__)
    instruction_set = kThumb2;
#endif
    runtime_->SetJniDlsymLookupStub(Compiler::CreateJniDlsymLookupStub(instruction_set));
    runtime_->SetAbstractMethodErrorStubArray(Compiler::CreateAbstractMethodErrorStub(instruction_set));
    for (int i = 0; i < Runtime::kLastTrampolineMethodType; i++) {
      Runtime::TrampolineType type = Runtime::TrampolineType(i);
      if (!runtime_->HasResolutionStubArray(type)) {
        runtime_->SetResolutionStubArray(
            Compiler::CreateResolutionStub(instruction_set, type), type);
      }
    }
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
      if (!runtime_->HasCalleeSaveMethod(type)) {
        runtime_->SetCalleeSaveMethod(
            runtime_->CreateCalleeSaveMethod(instruction_set, type), type);
      }
    }
    compiler_.reset(new Compiler(instruction_set, false, NULL));

    Heap::VerifyHeap();  // Check for heap corruption before the test
  }

  virtual void TearDown() {
    const char* android_data = getenv("ANDROID_DATA");
    ASSERT_TRUE(android_data != NULL);
    DIR* dir = opendir(art_cache_.c_str());
    ASSERT_TRUE(dir != NULL);
    while (true) {
      struct dirent entry;
      struct dirent* entry_ptr;
      int readdir_result = readdir_r(dir, &entry, &entry_ptr);
      ASSERT_EQ(0, readdir_result);
      if (entry_ptr == NULL) {
        break;
      }
      if ((strcmp(entry_ptr->d_name, ".") == 0) || (strcmp(entry_ptr->d_name, "..") == 0)) {
        continue;
      }
      std::string filename(art_cache_);
      filename.push_back('/');
      filename.append(entry_ptr->d_name);
      int unlink_result = unlink(filename.c_str());
      ASSERT_EQ(0, unlink_result);
    }
    closedir(dir);
    int rmdir_cache_result = rmdir(art_cache_.c_str());
    ASSERT_EQ(0, rmdir_cache_result);
    int rmdir_data_result = rmdir(android_data_.c_str());
    ASSERT_EQ(0, rmdir_data_result);

    // icu4c has a fixed 10-element array "gCommonICUDataArray".
    // If we run > 10 tests, we fill that array and u_setCommonData fails.
    // There's a function to clear the array, but it's not public...
    typedef void (*IcuCleanupFn)();
    void* sym = dlsym(RTLD_DEFAULT, "u_cleanup_" U_ICU_VERSION_SHORT);
    CHECK(sym != NULL);
    IcuCleanupFn icu_cleanup_fn = reinterpret_cast<IcuCleanupFn>(sym);
    (*icu_cleanup_fn)();

    compiler_.reset();
    STLDeleteElements(&opened_dex_files_);

    Heap::VerifyHeap();  // Check for heap corruption after the test
  }

  std::string GetLibCoreDexFileName() {
    if (is_host_) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != NULL);
      return StringPrintf("%s/framework/core-hostdex.jar", host_dir);
    }
    return std::string("/system/framework/core.jar");
  }

  const DexFile* OpenTestDexFile(const char* name) {
    CHECK(name != NULL);
    std::string filename;
    if (is_host_) {
      // on the host, just read target dex file
      filename += getenv("ANDROID_PRODUCT_OUT");
    }
    filename += "/data/art-test/art-test-dex-";
    filename += name;
    filename += ".jar";
    const DexFile* dex_file = DexFile::Open(filename, "");
    CHECK(dex_file != NULL) << "Failed to open " << filename;
    opened_dex_files_.push_back(dex_file);
    return dex_file;
  }

  ClassLoader* LoadDex(const char* dex_name) {
    const DexFile* dex_file = OpenTestDexFile(dex_name);
    CHECK(dex_file != NULL);
    class_linker_->RegisterDexFile(*dex_file);
    std::vector<const DexFile*> class_path;
    class_path.push_back(dex_file);
    SirtRef<ClassLoader> class_loader(PathClassLoader::AllocCompileTime(class_path));
    CHECK(class_loader.get() != NULL);
    Thread::Current()->SetClassLoaderOverride(class_loader.get());
    return class_loader.get();
  }

  void CompileClass(const ClassLoader* class_loader, const char* class_name) {
    std::string class_descriptor(DotToDescriptor(class_name));
    Class* klass = class_linker_->FindClass(class_descriptor.c_str(), class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
      CompileMethod(klass->GetDirectMethod(i));
    }
    for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
      CompileMethod(klass->GetVirtualMethod(i));
    }
  }

  void CompileMethod(Method* method) {
    CHECK(method != NULL);
    compiler_->CompileOne(method);
    MakeExecutable(method);

    MakeExecutable(runtime_->GetJniDlsymLookupStub());
  }

  void CompileDirectMethod(ClassLoader* class_loader,
                           const char* class_name,
                           const char* method_name,
                           const char* signature) {
    std::string class_descriptor(DotToDescriptor(class_name));
    Class* klass = class_linker_->FindClass(class_descriptor.c_str(), class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    Method* method = klass->FindDirectMethod(method_name, signature);
    CHECK(method != NULL) << "Direct method not found: "
                          << class_name << "." << method_name << signature;
    CompileMethod(method);
  }

  void CompileVirtualMethod(ClassLoader* class_loader,
                            const char* class_name,
                            const char* method_name,
                            const char* signature) {
    std::string class_descriptor(DotToDescriptor(class_name));
    Class* klass = class_linker_->FindClass(class_descriptor.c_str(), class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    Method* method = klass->FindVirtualMethod(method_name, signature);
    CHECK(method != NULL) << "Virtual method not found: "
                          << class_name << "." << method_name << signature;
    CompileMethod(method);
  }

  bool is_host_;
  std::string android_data_;
  std::string art_cache_;
  UniquePtr<const DexFile> java_lang_dex_file_;
  std::vector<const DexFile*> boot_class_path_;
  UniquePtr<Runtime> runtime_;
  // Owned by the runtime
  ClassLinker* class_linker_;
  UniquePtr<Compiler> compiler_;

 private:
  std::vector<const DexFile*> opened_dex_files_;
};

}  // namespace art

namespace std {

// TODO: isn't gtest supposed to be able to print STL types for itself?
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& rhs) {
  os << ::art::ToString(rhs);
  return os;
}

}  // namespace std
