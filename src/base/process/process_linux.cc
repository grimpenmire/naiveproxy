// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/process/process.h"

#include <errno.h>
#include <sys/resource.h>

#include <cstring>

#include "base/check.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/notreached.h"
#include "base/posix/can_lower_nice_to.h"
#include "base/process/internal_linux.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"

#if BUILDFLAG(IS_CHROMEOS)
#include "base/bind.h"
#include "base/feature_list.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/unguessable_token.h"
#endif  // BUILDFLAG(IS_CHROMEOS)

namespace base {

#if BUILDFLAG(IS_CHROMEOS)
const Feature kOneGroupPerRenderer {
  "OneGroupPerRenderer",

#if BUILDFLAG(IS_CHROMEOS_LACROS)
      FEATURE_ENABLED_BY_DEFAULT
};
#else
      FEATURE_DISABLED_BY_DEFAULT
};
#endif  // BUILDFLAG(IS_CHROMEOS_LACROS)
#endif  // BUILDFLAG(IS_CHROMEOS)

namespace {

const int kForegroundPriority = 0;

#if BUILDFLAG(IS_CHROMEOS)
// We are more aggressive in our lowering of background process priority
// for chromeos as we have much more control over other processes running
// on the machine.
//
// TODO(davemoore) Refactor this by adding support for higher levels to set
// the foregrounding / backgrounding process so we don't have to keep
// chrome / chromeos specific logic here.
const int kBackgroundPriority = 19;
const char kControlPath[] = "/sys/fs/cgroup/cpu%s/cgroup.procs";
const char kFullRendererCgroupRoot[] = "/sys/fs/cgroup/cpu/chrome_renderers";
const char kForeground[] = "/chrome_renderers/foreground";
const char kBackground[] = "/chrome_renderers/background";
const char kProcPath[] = "/proc/%d/cgroup";
const char kUclampMinFile[] = "cpu.uclamp.min";
const char kUclampMaxFile[] = "cpu.uclamp.max";

constexpr int kCgroupDeleteRetries = 3;
constexpr TimeDelta kCgroupDeleteRetryTime(Seconds(1));

#if BUILDFLAG(IS_CHROMEOS_LACROS)
const char kCgroupPrefix[] = "l-";
#elif BUILDFLAG(IS_CHROMEOS_ASH)
const char kCgroupPrefix[] = "a-";
#endif

struct CGroups {
  // Check for cgroups files. ChromeOS supports these by default. It creates
  // a cgroup mount in /sys/fs/cgroup and then configures two cpu task groups,
  // one contains at most a single foreground renderer and the other contains
  // all background renderers. This allows us to limit the impact of background
  // renderers on foreground ones to a greater level than simple renicing.
  bool enabled;
  FilePath foreground_file;
  FilePath background_file;

  // A unique token for this instance of the browser.
  std::string group_prefix_token;

  // UCLAMP settings for the foreground cgroups.
  std::string uclamp_min;
  std::string uclamp_max;

  CGroups() {
    foreground_file = FilePath(StringPrintf(kControlPath, kForeground));
    background_file = FilePath(StringPrintf(kControlPath, kBackground));
    FileSystemType foreground_type;
    FileSystemType background_type;
    enabled = GetFileSystemType(foreground_file, &foreground_type) &&
              GetFileSystemType(background_file, &background_type) &&
              foreground_type == FILE_SYSTEM_CGROUP &&
              background_type == FILE_SYSTEM_CGROUP;

    if (!enabled || !FeatureList::IsEnabled(kOneGroupPerRenderer)) {
      return;
    }

    // Generate a unique token for the full browser process
    group_prefix_token =
        StrCat({kCgroupPrefix, UnguessableToken::Create().ToString(), "-"});

    // Reads the ULCAMP settings from the foreground cgroup that will be used
    // for each renderer's cgroup.
    FilePath foreground_path = foreground_file.DirName();
    ReadFileToString(foreground_path.Append(kUclampMinFile), &uclamp_min);
    ReadFileToString(foreground_path.Append(kUclampMaxFile), &uclamp_max);
  }

  // Returns the full path to a the cgroup dir of a process using
  // the supplied token.
  static FilePath GetForegroundCgroupDir(const std::string& token) {
    // Get individualized cgroup if the feature is enabled
    std::string cgroup_path_str;
    StrAppend(&cgroup_path_str, {kFullRendererCgroupRoot, "/", token});
    return FilePath(cgroup_path_str);
  }

  // Returns the path to the cgroup.procs file of the foreground cgroup.
  static FilePath GetForegroundCgroupFile(const std::string& token) {
    // Processes with an empty token use the default foreground cgroup.
    if (token.empty()) {
      return CGroups::Get().foreground_file;
    }

    FilePath cgroup_path = GetForegroundCgroupDir(token);
    return cgroup_path.Append("cgroup.procs");
  }

  static CGroups& Get() {
    static auto& groups = *new CGroups;
    return groups;
  }
};

// Returns true if the 'OneGroupPerRenderer' feature is enabled. The feature
// is enabled if the kOneGroupPerRenderer feature flag is enabled and the
// system supports the chrome cgroups. Will block if this is the first call
// that will read the cgroup configs.
bool OneGroupPerRendererEnabled() {
  return FeatureList::IsEnabled(kOneGroupPerRenderer) && CGroups::Get().enabled;
}
#else
const int kBackgroundPriority = 5;
#endif  // BUILDFLAG(IS_CHROMEOS)

}  // namespace

Time Process::CreationTime() const {
  int64_t start_ticks = is_current()
                            ? internal::ReadProcSelfStatsAndGetFieldAsInt64(
                                  internal::VM_STARTTIME)
                            : internal::ReadProcStatsAndGetFieldAsInt64(
                                  Pid(), internal::VM_STARTTIME);

  if (!start_ticks)
    return Time();

  TimeDelta start_offset = internal::ClockTicksToTimeDelta(start_ticks);
  Time boot_time = internal::GetBootTime();
  if (boot_time.is_null())
    return Time();
  return Time(boot_time + start_offset);
}

// static
bool Process::CanBackgroundProcesses() {
#if BUILDFLAG(IS_CHROMEOS)
  if (CGroups::Get().enabled)
    return true;
#endif  // BUILDFLAG(IS_CHROMEOS)

  static const bool can_reraise_priority =
      internal::CanLowerNiceTo(kForegroundPriority);
  return can_reraise_priority;
}

bool Process::IsProcessBackgrounded() const {
  DCHECK(IsValid());

#if BUILDFLAG(IS_CHROMEOS)
  if (CGroups::Get().enabled) {
    // Used to allow reading the process priority from proc on thread launch.
    ThreadRestrictions::ScopedAllowIO allow_io;
    std::string proc;
    if (ReadFileToString(FilePath(StringPrintf(kProcPath, process_)), &proc)) {
      return IsProcessBackgroundedCGroup(proc);
    }
    return false;
  }
#endif  // BUILDFLAG(IS_CHROMEOS)

  return GetPriority() == kBackgroundPriority;
}

bool Process::SetProcessBackgrounded(bool background) {
  DCHECK(IsValid());

#if BUILDFLAG(IS_CHROMEOS)
  if (CGroups::Get().enabled) {
    std::string pid = NumberToString(process_);
    const FilePath file =
        background ? CGroups::Get().background_file
                   : CGroups::Get().GetForegroundCgroupFile(unique_token_);
    return WriteFile(file, pid);
  }
#endif  // BUILDFLAG(IS_CHROMEOS)

  if (!CanBackgroundProcesses())
    return false;

  int priority = background ? kBackgroundPriority : kForegroundPriority;
  int result = setpriority(PRIO_PROCESS, static_cast<id_t>(process_), priority);
  DPCHECK(result == 0);
  return result == 0;
}

#if BUILDFLAG(IS_CHROMEOS)
bool IsProcessBackgroundedCGroup(const StringPiece& cgroup_contents) {
  // The process can be part of multiple control groups, and for each cgroup
  // hierarchy there's an entry in the file. We look for a control group
  // named "/chrome_renderers/background" to determine if the process is
  // backgrounded. crbug.com/548818.
  std::vector<StringPiece> lines = SplitStringPiece(
      cgroup_contents, "\n", TRIM_WHITESPACE, SPLIT_WANT_NONEMPTY);
  for (const auto& line : lines) {
    std::vector<StringPiece> fields =
        SplitStringPiece(line, ":", TRIM_WHITESPACE, SPLIT_WANT_ALL);
    if (fields.size() != 3U) {
      NOTREACHED();
      continue;
    }
    if (fields[2] == kBackground)
      return true;
  }

  return false;
}
#endif  // BUILDFLAG(IS_CHROMEOS)

#if BUILDFLAG(IS_CHROMEOS_ASH)
// Reads /proc/<pid>/status and returns the PID in its PID namespace.
// If the process is not in a PID namespace or /proc/<pid>/status does not
// report NSpid, kNullProcessId is returned.
ProcessId Process::GetPidInNamespace() const {
  std::string status;
  {
    // Synchronously reading files in /proc does not hit the disk.
    ThreadRestrictions::ScopedAllowIO allow_io;
    FilePath status_file =
        FilePath("/proc").Append(NumberToString(process_)).Append("status");
    if (!ReadFileToString(status_file, &status)) {
      return kNullProcessId;
    }
  }

  StringPairs pairs;
  SplitStringIntoKeyValuePairs(status, ':', '\n', &pairs);
  for (const auto& pair : pairs) {
    const std::string& key = pair.first;
    const std::string& value_str = pair.second;
    if (key == "NSpid") {
      std::vector<StringPiece> split_value_str = SplitStringPiece(
          value_str, "\t", TRIM_WHITESPACE, SPLIT_WANT_NONEMPTY);
      if (split_value_str.size() <= 1) {
        return kNullProcessId;
      }
      int value;
      // The last value in the list is the PID in the namespace.
      if (!StringToInt(split_value_str.back(), &value)) {
        NOTREACHED();
        return kNullProcessId;
      }
      return value;
    }
  }
  return kNullProcessId;
}
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)

#if BUILDFLAG(IS_CHROMEOS)
// static
bool Process::OneGroupPerRendererEnabledForTesting() {
  return OneGroupPerRendererEnabled();
}

// On Chrome OS, each renderer runs in its own cgroup when running in the
// foreground. After process creation the cgroup is created using a
// unique token.
void Process::InitializePriority() {
  if (!OneGroupPerRendererEnabled() || !IsValid() || !unique_token_.empty()) {
    return;
  }

  // The token has the following format:
  //   {cgroup_prefix}{UnguessableToken}
  // The cgroup prefix is to distinguish ash from lacros tokens for stale
  // cgroup cleanup.
  unique_token_ = StrCat({CGroups::Get().group_prefix_token,
                          UnguessableToken::Create().ToString()});

  FilePath cgroup_path = CGroups::Get().GetForegroundCgroupDir(unique_token_);
  // Note that CreateDirectoryAndGetError() does not fail if the directory
  // already exits.
  if (!CreateDirectoryAndGetError(cgroup_path, nullptr)) {
    // If creating the directory fails, fall back to use the foreground group.
    int saved_errno = errno;
    LOG(ERROR) << "Failed to create cgroup, falling back to foreground"
               << ", cgroup=" << cgroup_path
               << ", errno=" << strerror(saved_errno);

    unique_token_.clear();
    return;
  }

  if (!CGroups::Get().uclamp_min.empty() &&
      !WriteFile(cgroup_path.Append(kUclampMinFile),
                 CGroups::Get().uclamp_min)) {
    LOG(ERROR) << "Failed to write uclamp min file, cgroup_path="
               << cgroup_path;
  }
  if (!CGroups::Get().uclamp_min.empty() &&
      !WriteFile(cgroup_path.Append(kUclampMaxFile),
                 CGroups::Get().uclamp_max)) {
    LOG(ERROR) << "Failed to write uclamp max file, cgroup_path="
               << cgroup_path;
  }
}

// static
void Process::CleanUpProcessScheduled(Process process, int remaining_retries) {
  process.CleanUpProcess(remaining_retries);
}

void Process::CleanUpProcessAsync() const {
  if (!FeatureList::IsEnabled(kOneGroupPerRenderer) || unique_token_.empty()) {
    return;
  }

  ThreadPool::PostTask(FROM_HERE, {MayBlock(), TaskPriority::BEST_EFFORT},
                       BindOnce(&Process::CleanUpProcessScheduled, Duplicate(),
                                kCgroupDeleteRetries));
}

void Process::CleanUpProcess(int remaining_retries) const {
  if (!OneGroupPerRendererEnabled() || unique_token_.empty()) {
    return;
  }

  // Try to delete the cgroup
  // TODO(1322562): We can use notify_on_release to automoatically delete the
  // cgroup when the process has left the cgroup.
  FilePath cgroup = CGroups::Get().GetForegroundCgroupDir(unique_token_);
  if (!DeleteFile(cgroup)) {
    auto saved_errno = errno;
    LOG(ERROR) << "Failed to delete cgroup " << cgroup
               << ", errno=" << strerror(saved_errno);
    // If the delete failed, then the process is still potentially in the
    // cgroup. Move the process to background and schedule a callback to try
    // again.
    if (remaining_retries > 0) {
      std::string pidstr = NumberToString(process_);
      if (!WriteFile(CGroups::Get().background_file, pidstr)) {
        // Failed to move the process, LOG a warning but try again.
        saved_errno = errno;
        LOG(WARNING) << "Failed to move the process to background"
                     << ", pid=" << pidstr
                     << ", errno=" << strerror(saved_errno);
      }
      ThreadPool::PostDelayedTask(FROM_HERE,
                                  {MayBlock(), TaskPriority::BEST_EFFORT},
                                  BindOnce(&Process::CleanUpProcessScheduled,
                                           Duplicate(), remaining_retries - 1),
                                  kCgroupDeleteRetryTime);
    }
  }
}

// static
void Process::CleanUpStaleProcessStates() {
  if (!OneGroupPerRendererEnabled()) {
    return;
  }

  FileEnumerator traversal(FilePath(kFullRendererCgroupRoot), false,
                           FileEnumerator::DIRECTORIES);
  for (FilePath path = traversal.Next(); !path.empty();
       path = traversal.Next()) {
    std::string dirname = path.BaseName().value();
    if (dirname == FilePath(kForeground).BaseName().value() ||
        dirname == FilePath(kBackground).BaseName().value()) {
      continue;
    }

    if (!StartsWith(dirname, kCgroupPrefix) ||
        StartsWith(dirname, CGroups::Get().group_prefix_token)) {
      continue;
    }

    if (!DeleteFile(path)) {
      auto saved_errno = errno;
      LOG(ERROR) << "Failed to delete " << path
                 << ", errno=" << strerror(saved_errno);
    }
  }
}
#endif  // BUILDFLAG(IS_CHROMEOS)

}  // namespace base
