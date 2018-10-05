// Copyright 2017 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "handler/minidump_to_upload_parameters.h"

#include "base/strings/string_number_conversions.h"
#include "base/logging.h"
#include "client/annotation.h"
#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "client/client_argv_handling.h"
#include "snapshot/module_snapshot.h"
#include "snapshot/process_snapshot.h"
#include "util/stdlib/map_insert.h"

namespace crashpad {

namespace {

void InsertOrReplaceMapEntry(std::map<std::string, std::string>* map,
                             const std::string& key,
                             const std::string& value) {
  std::string old_value;
  if (!MapInsertOrReplace(map, key, value, &old_value)) {
    LOG(WARNING) << "duplicate key " << key << ", discarding value "
                 << old_value;
  }
}

}  // namespace

int64_t GetAnnotationInt64(const std::string& name, int64_t defval) {
  CrashpadInfo* crashpad_info = CrashpadInfo::GetCrashpadInfo();
  SimpleStringDictionary* annotations = crashpad_info->simple_annotations();
  int64_t retval = defval;
  if (annotations) {
    const char *str = annotations->GetValueForKey(name);
    if (str) {
       base::StringToInt64(std::string(str), &retval);
    }
  }
  return retval;
}

std::string GetAnnotationString(const std::string& name) {
  CrashpadInfo* crashpad_info = CrashpadInfo::GetCrashpadInfo();
  SimpleStringDictionary* annotations = crashpad_info->simple_annotations();
  std::string retval;
  if (annotations) {
    const char *str = annotations->GetValueForKey(name);
    if (str) {
       retval = std::string(str);
    }
  }
  return retval;
}

std::map<std::string, std::string> BreakpadHTTPFormParametersFromMinidump(
    const ProcessSnapshot* process_snapshot) {
  std::map<std::string, std::string> parameters =
      process_snapshot->AnnotationsSimpleMap();

  std::string list_annotations;
  for (const ModuleSnapshot* module : process_snapshot->Modules()) {
    for (const auto& kv : module->AnnotationsSimpleMap()) {
      if (!parameters.insert(kv).second) {
        LOG(WARNING) << "duplicate key " << kv.first << ", discarding value "
                     << kv.second;
      }
    }

    for (const std::string& annotation : module->AnnotationsVector()) {
      list_annotations.append(annotation);
      list_annotations.append("\n");
    }

    for (const AnnotationSnapshot& annotation : module->AnnotationObjects()) {
      if (annotation.type != static_cast<uint16_t>(Annotation::Type::kString)) {
        continue;
      }

      std::string value(reinterpret_cast<const char*>(annotation.value.data()),
                        annotation.value.size());
      std::pair<std::string, std::string> entry(annotation.name, value);
      if (!parameters.insert(entry).second) {
        LOG(WARNING) << "duplicate annotation name " << annotation.name
                     << ", discarding value " << value;
      }
    }
  }

  if (!list_annotations.empty()) {
    // Remove the final newline character.
    list_annotations.resize(list_annotations.size() - 1);

    InsertOrReplaceMapEntry(&parameters, "list_annotations", list_annotations);
  }

  UUID client_id;
  process_snapshot->ClientID(&client_id);
  InsertOrReplaceMapEntry(&parameters, "guid", client_id.ToString());

  return parameters;
}

int64_t CrashpadUploadAttachmentFileSizeLimit(int default_kbytes) {
  return GetAnnotationInt64("UploadAttachmentKiloByteLimit", default_kbytes) * 1000L;
}

int CrashpadUploadPercentage(int default_percentage) {
  return (int) GetAnnotationInt64("UploadPercentage", default_percentage);
}

bool CrashpadUploadMiniDump() {
  return GetAnnotationString("Format") == "minidump";
}

#if defined(OS_LINUX)
bool MakeAdditionalTracerParameter(
  std::string& tracer_pathname,
  std::vector<std::string>& args,
  std::vector<const char*>& argv,
  pid_t tracee, const std::string& outfile) {

  CrashpadInfo* crashpad_info = CrashpadInfo::GetCrashpadInfo();
  SimpleStringDictionary* annotations = crashpad_info->simple_annotations();
  if (!annotations) {
    LOG(ERROR) << "The annotation dictionary does not exist";
    return false;
  }

  std::map<std::string, int> imap;
  for (int i = 0; i < (int)args.size(); ++i) {
    imap[args[i].substr(0, args[i].find("="))] = i;
  }
  int kvcnt = 0;

  SimpleStringDictionary::Iterator iter(*annotations);
  for (;;) {
    const SimpleStringDictionary::Entry *entry = iter.Next();
    if (!entry) {
      break;
    }

    std::string key(entry->key);
    if (key.find("--additional-tracer-opt") == 0) {
      /* Tracer options to add or overwrite previous value */
      std::string val = key.substr(key.find("=")+1);
      std::string opt = val.substr(0, val.find("="));
      if (imap.count(opt)) {
        args[imap[opt]] = val;
        LOG(INFO) << "Replace tracer argument [" << imap[opt] << "] : " << val;
      } else {
        args.push_back(val);
        LOG(INFO) << "Add a tracer argument: " << val;
      }
    } else if (key.find("--additional-tracer") == 0) {
      tracer_pathname = key.substr(key.find("=")+1);
      LOG(INFO) << "Replace tracer pathname: " << tracer_pathname;
    } else if (key.find("_mod_faulting_tid") == 0) {
      args.push_back("--fault-thread="  + std::string(entry->value));
    } else {
      args.push_back("--kv=" + std::string(entry->key)
                       + ":" + std::string(entry->value));
      ++kvcnt;
    }
  }

  if (!kvcnt) {
    LOG(ERROR) << "The annotation dictionary is empty";
    return false;
  }

  if (!outfile.empty()) {
    args.push_back("--output=" + outfile);
  }
  args.push_back(std::to_string(tracee));
  StringVectorToCStringVector(args, &argv);
  return true;
}
#endif

}  // namespace crashpad
