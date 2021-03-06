/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <thread>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/android_reboot.h>
#include <cutils/sockets.h>
#include <selinux/android.h>

#include "reboot.h"

#ifdef _INIT_INIT_H
#error "Do not include init.h in files used by ueventd or watchdogd; it will expose init's globals"
#endif

using android::base::boot_clock;
using namespace std::literals::string_literals;

namespace android {
namespace init {

const std::string kDefaultAndroidDtDir("/proc/device-tree/firmware/android/");

// DecodeUid() - decodes and returns the given string, which can be either the
// numeric or name representation, into the integer uid or gid. Returns
// UINT_MAX on error.
bool DecodeUid(const std::string& name, uid_t* uid, std::string* err) {
    *uid = UINT_MAX;
    *err = "";

    if (isalpha(name[0])) {
        passwd* pwd = getpwnam(name.c_str());
        if (!pwd) {
            *err = "getpwnam failed: "s + strerror(errno);
            return false;
        }
        *uid = pwd->pw_uid;
        return true;
    }

    errno = 0;
    uid_t result = static_cast<uid_t>(strtoul(name.c_str(), 0, 0));
    if (errno) {
        *err = "strtoul failed: "s + strerror(errno);
        return false;
    }
    *uid = result;
    return true;
}

/*
 * CreateSocket - creates a Unix domain socket in ANDROID_SOCKET_DIR
 * ("/dev/socket") as dictated in init.rc. This socket is inherited by the
 * daemon. We communicate the file descriptor's value via the environment
 * variable ANDROID_SOCKET_ENV_PREFIX<name> ("ANDROID_SOCKET_foo").
 */
int CreateSocket(const char* name, int type, bool passcred, mode_t perm, uid_t uid, gid_t gid,
                 const char* socketcon, selabel_handle* sehandle) {
    if (socketcon) {
        if (setsockcreatecon(socketcon) == -1) {
            PLOG(ERROR) << "setsockcreatecon(\"" << socketcon << "\") failed";
            return -1;
        }
    }

    android::base::unique_fd fd(socket(PF_UNIX, type, 0));
    if (fd < 0) {
        PLOG(ERROR) << "Failed to open socket '" << name << "'";
        return -1;
    }

    if (socketcon) setsockcreatecon(NULL);

    struct sockaddr_un addr;
    memset(&addr, 0 , sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), ANDROID_SOCKET_DIR"/%s",
             name);

    if ((unlink(addr.sun_path) != 0) && (errno != ENOENT)) {
        PLOG(ERROR) << "Failed to unlink old socket '" << name << "'";
        return -1;
    }

    char *filecon = NULL;
    if (sehandle) {
        if (selabel_lookup(sehandle, &filecon, addr.sun_path, S_IFSOCK) == 0) {
            setfscreatecon(filecon);
        }
    }

    if (passcred) {
        int on = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on))) {
            PLOG(ERROR) << "Failed to set SO_PASSCRED '" << name << "'";
            return -1;
        }
    }

    int ret = bind(fd, (struct sockaddr *) &addr, sizeof (addr));
    int savederrno = errno;

    setfscreatecon(NULL);
    freecon(filecon);

    if (ret) {
        errno = savederrno;
        PLOG(ERROR) << "Failed to bind socket '" << name << "'";
        goto out_unlink;
    }

    if (lchown(addr.sun_path, uid, gid)) {
        PLOG(ERROR) << "Failed to lchown socket '" << addr.sun_path << "'";
        goto out_unlink;
    }
    if (fchmodat(AT_FDCWD, addr.sun_path, perm, AT_SYMLINK_NOFOLLOW)) {
        PLOG(ERROR) << "Failed to fchmodat socket '" << addr.sun_path << "'";
        goto out_unlink;
    }

    LOG(INFO) << "Created socket '" << addr.sun_path << "'"
              << ", mode " << std::oct << perm << std::dec
              << ", user " << uid
              << ", group " << gid;

    return fd.release();

out_unlink:
    unlink(addr.sun_path);
    return -1;
}

bool ReadFile(const std::string& path, std::string* content, std::string* err) {
    content->clear();
    *err = "";

    android::base::unique_fd fd(
        TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)));
    if (fd == -1) {
        *err = "Unable to open '" + path + "': " + strerror(errno);
        return false;
    }

    // For security reasons, disallow world-writable
    // or group-writable files.
    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        *err = "fstat failed for '" + path + "': " + strerror(errno);
        return false;
    }
    if ((sb.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        *err = "Skipping insecure file '" + path + "'";
        return false;
    }

    if (!android::base::ReadFdToString(fd, content)) {
        *err = "Unable to read '" + path + "': " + strerror(errno);
        return false;
    }
    return true;
}

bool WriteFile(const std::string& path, const std::string& content, std::string* err) {
    *err = "";

    android::base::unique_fd fd(TEMP_FAILURE_RETRY(
        open(path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0600)));
    if (fd == -1) {
        *err = "Unable to open '" + path + "': " + strerror(errno);
        return false;
    }
    if (!android::base::WriteStringToFd(content, fd)) {
        *err = "Unable to write to '" + path + "': " + strerror(errno);
        return false;
    }
    return true;
}

int mkdir_recursive(const std::string& path, mode_t mode, selabel_handle* sehandle) {
    std::string::size_type slash = 0;
    while ((slash = path.find('/', slash + 1)) != std::string::npos) {
        auto directory = path.substr(0, slash);
        struct stat info;
        if (stat(directory.c_str(), &info) != 0) {
            auto ret = make_dir(directory.c_str(), mode, sehandle);
            if (ret && errno != EEXIST) return ret;
        }
    }
    auto ret = make_dir(path.c_str(), mode, sehandle);
    if (ret && errno != EEXIST) return ret;
    return 0;
}

int wait_for_file(const char* filename, std::chrono::nanoseconds timeout) {
    boot_clock::time_point timeout_time = boot_clock::now() + timeout;
    while (boot_clock::now() < timeout_time) {
        struct stat sb;
        if (stat(filename, &sb) != -1) return 0;

        std::this_thread::sleep_for(10ms);
    }
    return -1;
}

void import_kernel_cmdline(bool in_qemu,
                           const std::function<void(const std::string&, const std::string&, bool)>& fn) {
    std::string cmdline;
    android::base::ReadFileToString("/proc/cmdline", &cmdline);

    for (const auto& entry : android::base::Split(android::base::Trim(cmdline), " ")) {
        std::vector<std::string> pieces = android::base::Split(entry, "=");
        if (pieces.size() == 2) {
            fn(pieces[0], pieces[1], in_qemu);
        }
    }
}

int make_dir(const char* path, mode_t mode, selabel_handle* sehandle) {
    int rc;

    char *secontext = NULL;

    if (sehandle) {
        selabel_lookup(sehandle, &secontext, path, mode);
        setfscreatecon(secontext);
    }

    rc = mkdir(path, mode);

    if (secontext) {
        int save_errno = errno;
        freecon(secontext);
        setfscreatecon(NULL);
        errno = save_errno;
    }

    return rc;
}

/*
 * Writes hex_len hex characters (1/2 byte) to hex from bytes.
 */
std::string bytes_to_hex(const uint8_t* bytes, size_t bytes_len) {
    std::string hex("0x");
    for (size_t i = 0; i < bytes_len; i++)
        android::base::StringAppendF(&hex, "%02x", bytes[i]);
    return hex;
}

/*
 * Returns true is pathname is a directory
 */
bool is_dir(const char* pathname) {
    struct stat info;
    if (stat(pathname, &info) == -1) {
        return false;
    }
    return S_ISDIR(info.st_mode);
}

bool expand_props(const std::string& src, std::string* dst) {
    const char* src_ptr = src.c_str();

    if (!dst) {
        return false;
    }

    /* - variables can either be $x.y or ${x.y}, in case they are only part
     *   of the string.
     * - will accept $$ as a literal $.
     * - no nested property expansion, i.e. ${foo.${bar}} is not supported,
     *   bad things will happen
     * - ${x.y:-default} will return default value if property empty.
     */
    while (*src_ptr) {
        const char* c;

        c = strchr(src_ptr, '$');
        if (!c) {
            dst->append(src_ptr);
            return true;
        }

        dst->append(src_ptr, c);
        c++;

        if (*c == '$') {
            dst->push_back(*(c++));
            src_ptr = c;
            continue;
        } else if (*c == '\0') {
            return true;
        }

        std::string prop_name;
        std::string def_val;
        if (*c == '{') {
            c++;
            const char* end = strchr(c, '}');
            if (!end) {
                // failed to find closing brace, abort.
                LOG(ERROR) << "unexpected end of string in '" << src << "', looking for }";
                return false;
            }
            prop_name = std::string(c, end);
            c = end + 1;
            size_t def = prop_name.find(":-");
            if (def < prop_name.size()) {
                def_val = prop_name.substr(def + 2);
                prop_name = prop_name.substr(0, def);
            }
        } else {
            prop_name = c;
            LOG(ERROR) << "using deprecated syntax for specifying property '" << c << "', use ${name} instead";
            c += prop_name.size();
        }

        if (prop_name.empty()) {
            LOG(ERROR) << "invalid zero-length property name in '" << src << "'";
            return false;
        }

        std::string prop_val = android::base::GetProperty(prop_name, "");
        if (prop_val.empty()) {
            if (def_val.empty()) {
                LOG(ERROR) << "property '" << prop_name << "' doesn't exist while expanding '" << src << "'";
                return false;
            }
            prop_val = def_val;
        }

        dst->append(prop_val);
        src_ptr = c;
    }

    return true;
}

void panic() {
    LOG(ERROR) << "panic: rebooting to bootloader";
    // Do not queue "shutdown" trigger since we want to shutdown immediately
    DoReboot(ANDROID_RB_RESTART2, "reboot", "bootloader", false);
}

static void create_dt_file_by_cmdline(const std::string& key, const std::string& value, bool for_emulator, std::string rootdir) {
    std::string file_path,file_content,error_ret;
    if (key.empty()) return;

//    if (for_emulator) {
        // In the emulator
//        return;
//    }
    if (android::base::StartsWith(key, "android.fw.")) {
        file_path = android::base::StringPrintf("%s/%s", rootdir.c_str(), key.c_str() + 11);
        std::replace(file_path.begin(), file_path.end(), '.', '/');
        mkdir_recursive(android::base::Dirname(file_path).c_str(), 0700, NULL);
        file_content = value + android::base::StringPrintf("\n");
        WriteFile(file_path.c_str(), file_content.c_str(),&error_ret);
    }
}
static std::string get_acpi_cfg_path_from_cmdline()
{
    std::string cmdline;
    android::base::ReadFileToString("/proc/cmdline", &cmdline);

    for (const auto& entry : android::base::Split(android::base::Trim(cmdline), " ")) {
        std::vector<std::string> pieces = android::base::Split(entry, "=");
        if ((pieces.size() == 2) && ("android.acpi.cfg.root" == pieces[0])) {
               return pieces[1]; //find path of acpi cfg in kernel command line
        }
    }
    return "/sys/devices/system/container/ACPI0004:00/firmware_node"; //return default path
}
static bool import_acpi_cmdline(bool in_qemu,
                                          const std::function<void(const std::string&, const std::string&, bool, std::string)>& fn, std::string rootdir) {
    std::string cmdline;
    std::string acpi_cfg_path = get_acpi_cfg_path_from_cmdline();
    LOG(INFO)<<"acpi cfg root:"<< acpi_cfg_path;
    android::base::ReadFileToString(acpi_cfg_path+"/path",&cmdline);
    std::size_t found = cmdline.find("CFG0");       //verify acpi device name
    if (found == std::string::npos) return false;   //ACPI configuire file doesn't exist

    android::base::ReadFileToString(acpi_cfg_path+"/description", &cmdline);
    std::replace(cmdline.begin(), cmdline.end(), '\n', ' ');

    for (const auto& entry : android::base::Split(android::base::Trim(cmdline), " ")) {
        std::vector<std::string> pieces = android::base::Split(entry, "=");
        if (pieces.size() == 2) {
            fn(pieces[0], pieces[1], in_qemu,rootdir);
        }
    }
    return true;
}



static std::string init_android_dt_dir() {
    // Use the standard procfs-based path by default
    std::string android_dt_dir = kDefaultAndroidDtDir;
    if(!is_dir(kDefaultAndroidDtDir.c_str())){  //if Device-Tree exist, ignore other configure.
        // The platform may specify a custom Android DT path in kernel cmdline
        import_kernel_cmdline(false,
                              [&](const std::string& key, const std::string& value, bool in_qemu) {
                                  if (key == "androidboot.android_dt_dir") {
                                      android_dt_dir = value;
                                  }
                              });
        if(android_dt_dir == kDefaultAndroidDtDir)  //kernel cmdline doesn't set, we use a default path for alternative device-tree.
            android_dt_dir = "/dev/device-tree/firmware/android/";
        LOG(INFO) << "Using Android DT directory " << android_dt_dir;
        if(!is_dir(android_dt_dir.c_str())){
            //to create the alternative device tree in ramdisk if Kernel doesn't do it.
            import_acpi_cmdline(false, create_dt_file_by_cmdline,android_dt_dir);
        }
    }
    return android_dt_dir;
}

// FIXME: The same logic is duplicated in system/core/fs_mgr/
const std::string& get_android_dt_dir() {
    // Set once and saves time for subsequent calls to this function
    static const std::string kAndroidDtDir = init_android_dt_dir();
    return kAndroidDtDir;
}

// Reads the content of device tree file under the platform's Android DT directory.
// Returns true if the read is success, false otherwise.
bool read_android_dt_file(const std::string& sub_path, std::string* dt_content) {
    const std::string file_name = get_android_dt_dir() + sub_path;
    if (android::base::ReadFileToString(file_name, dt_content,true)) {
        if (!dt_content->empty()) {
            dt_content->pop_back();  // Trims the trailing '\0' out.
            return true;
        }
    }
    return false;
}

bool is_android_dt_value_expected(const std::string& sub_path, const std::string& expected_content) {
    std::string dt_content;
    if (read_android_dt_file(sub_path, &dt_content)) {
        if (dt_content == expected_content) {
            return true;
        }
    }
    return false;
}

}  // namespace init
}  // namespace android
