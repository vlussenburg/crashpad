// Copyright 2018 The Crashpad Authors. All rights reserved.
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

#include "handler/linux/crash_report_exception_handler.h"

#include <errno.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <vector>
#include <memory>
#include <utility>

#include "base/logging.h"
#include "client/settings.h"
#include "handler/linux/capture_snapshot.h"
#include "minidump/minidump_file_writer.h"
#include "snapshot/linux/process_snapshot_linux.h"
#include "snapshot/sanitized/process_snapshot_sanitized.h"
#include "util/file/file_helper.h"
#include "util/file/file_reader.h"
#include "util/file/output_stream_file_writer.h"
#include "util/linux/direct_ptrace_connection.h"
#include "util/linux/ptrace_client.h"
#include "util/misc/implicit_cast.h"
#include "util/misc/metrics.h"
#include "util/misc/uuid.h"
#include "util/stream/base94_output_stream.h"
#include "util/stream/log_output_stream.h"
#include "util/stream/zlib_output_stream.h"

#include "util/roblox/user_callback_functions.h"
#include "handler/minidump_to_upload_parameters.h"

namespace crashpad {

namespace {

bool WriteMinidumpLogFromFile(FileReaderInterface* file_reader) {
  ZlibOutputStream stream(ZlibOutputStream::Mode::kCompress,
                          std::make_unique<Base94OutputStream>(
                              Base94OutputStream::Mode::kEncode,
                              std::make_unique<LogOutputStream>()));
  FileOperationResult read_result;
  do {
    uint8_t buffer[4096];
    read_result = file_reader->Read(buffer, sizeof(buffer));
    if (read_result < 0)
      return false;

    if (read_result > 0 && (!stream.Write(buffer, read_result)))
      return false;
  } while (read_result > 0);
  return stream.Flush();
}

}  // namespace

CrashReportExceptionHandler::CrashReportExceptionHandler(
    CrashReportDatabase* database,
    CrashReportUploadThread* upload_thread,
    const std::map<std::string, std::string>* process_annotations,
    const std::map<std::string, base::FilePath>* process_attachments,
    bool write_minidump_to_database,
    bool write_minidump_to_log,
    const UserStreamDataSources* user_stream_data_sources)
    : database_(database),
      upload_thread_(upload_thread),
      process_annotations_(process_annotations),
      process_attachments_(process_attachments),
      write_minidump_to_database_(write_minidump_to_database),
      write_minidump_to_log_(write_minidump_to_log),
      user_stream_data_sources_(user_stream_data_sources){
  DCHECK(write_minidump_to_database_ | write_minidump_to_log_);
}

CrashReportExceptionHandler::~CrashReportExceptionHandler() = default;

bool CrashReportExceptionHandler::HandleException(
    pid_t client_process_id,
    uid_t client_uid,
    const ExceptionHandlerProtocol::ClientInformation& info,
    VMAddress requesting_thread_stack_address,
    pid_t* requesting_thread_id,
    UUID* local_report_id) {
  Metrics::ExceptionEncountered();

  DirectPtraceConnection connection;
  if (!connection.Initialize(client_process_id)) {
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kDirectPtraceFailed);
    return false;
  }

  return HandleExceptionWithConnection(&connection,
                                       info,
                                       client_uid,
                                       requesting_thread_stack_address,
                                       requesting_thread_id,
                                       local_report_id);
}

bool CrashReportExceptionHandler::HandleExceptionWithBroker(
    pid_t client_process_id,
    uid_t client_uid,
    const ExceptionHandlerProtocol::ClientInformation& info,
    int broker_sock,
    UUID* local_report_id) {
  Metrics::ExceptionEncountered();

  PtraceClient client;
  if (!client.Initialize(broker_sock, client_process_id)) {
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kBrokeredPtraceFailed);
    return false;
  }

  return HandleExceptionWithConnection(
      &client, info, client_uid, 0, nullptr, local_report_id);
}

bool CrashReportExceptionHandler::HandleExceptionWithConnection(
    PtraceConnection* connection,
    const ExceptionHandlerProtocol::ClientInformation& info,
    uid_t client_uid,
    VMAddress requesting_thread_stack_address,
    pid_t* requesting_thread_id,
    UUID* local_report_id) {
  std::unique_ptr<ProcessSnapshotLinux> process_snapshot;
  std::unique_ptr<ProcessSnapshotSanitized> sanitized_snapshot;
  if (!CaptureSnapshot(connection,
                       info,
                       *process_annotations_,
                       client_uid,
                       requesting_thread_stack_address,
                       requesting_thread_id,
                       &process_snapshot,
                       &sanitized_snapshot)) {
    return false;
  }

  RunUserCallbackOnDumpEvent(nullptr);
  UUID client_id;
  Settings* const settings = database_->GetSettings();
  if (settings) {
    // If GetSettings() or GetClientID() fails, something else will log a
    // message and client_id will be left at its default value, all zeroes,
    // which is appropriate.
    settings->GetClientID(&client_id);
  }
  process_snapshot->SetClientID(client_id);

  return write_minidump_to_database_
             ? WriteMinidumpToDatabase(process_snapshot.get(),
                                       sanitized_snapshot.get(),
                                       write_minidump_to_log_,
                                       local_report_id)
             : WriteMinidumpToLog(process_snapshot.get(),
                                  sanitized_snapshot.get());
}

bool CrashReportExceptionHandler::WriteMinidumpToDatabase(
    ProcessSnapshotLinux* process_snapshot,
    ProcessSnapshotSanitized* sanitized_snapshot,
    bool write_minidump_to_log,
    UUID* local_report_id) {
  std::unique_ptr<CrashReportDatabase::NewReport> new_report;
  CrashReportDatabase::OperationStatus database_status =
      database_->PrepareNewCrashReport(&new_report);
  if (database_status != CrashReportDatabase::kNoError) {
    LOG(ERROR) << "PrepareNewCrashReport failed";
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kPrepareNewCrashReportFailed);
    return false;
  }

  process_snapshot->SetReportID(new_report->ReportID());

  ProcessSnapshot* snapshot =
      sanitized_snapshot ? implicit_cast<ProcessSnapshot*>(sanitized_snapshot)
                         : implicit_cast<ProcessSnapshot*>(process_snapshot);

  MinidumpFileWriter minidump;
  minidump.InitializeFromSnapshot(snapshot);
  AddUserExtensionStreams(user_stream_data_sources_, snapshot, &minidump);

  if (!minidump.WriteEverything(new_report->Writer())) {
    LOG(ERROR) << "WriteEverything failed";
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kMinidumpWriteFailed);
    return false;
  }

  if (process_attachments_) {
    // Note that attachments are read at this point each time rather than once
    // so that if the contents of the file has changed it will be re-read for
    // each upload (e.g. in the case of a log file).
    for (const auto& it : *process_attachments_) {
      FileWriter* writer = new_report->AddAttachment(it.first);
      if (writer) {
        std::string contents;
        int64_t nBytes = CrashpadUploadAttachmentFileSizeLimit();
        if (!LoggingReadLastPartOfFile(it.second, &contents, (FileOffset)nBytes)) {
          // Not being able to read the file isn't considered fatal, and
          // should not prevent the report from being processed.
          continue;
        }
        writer->Write(contents.data(), contents.size());
      }
    }
  }

  bool write_minidump_to_log_succeed = false;
  if (write_minidump_to_log) {
    if (auto* file_reader = new_report->Reader()) {
      if (WriteMinidumpLogFromFile(file_reader))
        write_minidump_to_log_succeed = true;
      else
        LOG(ERROR) << "WriteMinidumpLogFromFile failed";
    }
  }

  UUID uuid;
  database_status =
      database_->FinishedWritingCrashReport(std::move(new_report), &uuid);
  if (database_status != CrashReportDatabase::kNoError) {
    LOG(ERROR) << "FinishedWritingCrashReport failed";
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kFinishedWritingCrashReportFailed);
    return false;
  }

  if (upload_thread_ && CrashpadUploadMiniDump()) {
    upload_thread_->ReportPending(uuid);
  }

  if (local_report_id != nullptr) {
    *local_report_id = uuid;
  }

  Metrics::ExceptionCaptureResult(Metrics::CaptureResult::kSuccess);

  return write_minidump_to_log ? write_minidump_to_log_succeed : true;
}

bool CrashReportExceptionHandler::WriteMinidumpToLog(
    ProcessSnapshotLinux* process_snapshot,
    ProcessSnapshotSanitized* sanitized_snapshot) {
  ProcessSnapshot* snapshot =
      sanitized_snapshot ? implicit_cast<ProcessSnapshot*>(sanitized_snapshot)
                         : implicit_cast<ProcessSnapshot*>(process_snapshot);
  MinidumpFileWriter minidump;
  minidump.InitializeFromSnapshot(snapshot);
  AddUserExtensionStreams(user_stream_data_sources_, snapshot, &minidump);

  OutputStreamFileWriter writer(std::make_unique<ZlibOutputStream>(
      ZlibOutputStream::Mode::kCompress,
      std::make_unique<Base94OutputStream>(
          Base94OutputStream::Mode::kEncode,
          std::make_unique<LogOutputStream>())));
  if (!minidump.WriteMinidump(&writer, false /* allow_seek */)) {
    LOG(ERROR) << "WriteMinidump failed";
    return false;
  }
  return writer.Flush();
}

// Roblox Addition: Backtrace ptrace tool on Linux only
#if defined(OS_LINUX)
bool CrashReportExceptionHandler::HandleExceptionWithAdditionalTracer(
    const base::FilePath& tracer_pathname,
    std::vector<std::string>& tracer_args,
    pid_t client_process_id,
    uid_t client_uid,
    const ExceptionHandlerProtocol::ClientInformation& info,
    UUID* local_report_id) {
  UUID report_uuid;
  if (!HandleException(client_process_id, client_uid, info, 0, nullptr, &report_uuid)) {
    return false;
  }
  if (local_report_id) {
    *local_report_id = report_uuid;
  }
  LOG(INFO) << "Crashpad generated report: " << report_uuid.ToString();

  if (CrashpadUploadMiniDump()) {
    LOG(INFO) << "Skip additional tracer, whose format is not minidump";
    return true;
  }

  CrashReportDatabase::Report report;
  if (database_->LookUpCrashReport(report_uuid, &report) !=
      CrashReportDatabase::kNoError) {
    LOG(ERROR) << "Failed to find report " << report_uuid.ToString();
    return false;
  }

  std::string fn = report.file_path.RemoveFinalExtension().value() + ".btt";
  std::string tracer(tracer_pathname.value());
  std::vector<const char*> argv;
  if (!MakeAdditionalTracerParameter(
      tracer, tracer_args, argv, client_process_id, fn)) {
    return false;
  }
  LOG(INFO) << "Start additional tracer with arguments:";
  for (auto v : tracer_args) {
    LOG(INFO) << v;
  }

  int status;
  pid_t pid_tracer = vfork();
  if (pid_tracer < 0) {
    return false;
  }
  if (pid_tracer == 0) {
    execv(tracer.c_str(), const_cast<char* const*>(argv.data()));
  }

  int result = waitpid(pid_tracer, &status, 0);
  if (result < 0) {
    LOG(ERROR) << tracer << " error: " << strerror(errno);
    LOG(ERROR) << tracer << " state: " << status;
    return false;
  }
  if (!WIFEXITED(status)) {
    LOG(ERROR) << tracer << " should have exited, but did not";
    if (WIFSTOPPED(status)) {
      LOG(ERROR) << tracer << " stopped on signal " << WSTOPSIG(status);
    }
    return false;
  }

  LOG(INFO) << "additional tracer succeed";
  if (upload_thread_) {
    LOG(INFO) << "uploading tracer report";
    upload_thread_->ReportPending(report_uuid);
    if (!upload_thread_->WaitForPendingUpload(60000)) {
      return false;
    }
  }

  LOG(INFO) << "Done uploading tracer report";
  return true;
}
#endif

}  // namespace crashpad
