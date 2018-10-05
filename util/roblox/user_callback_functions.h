#ifndef CRASHPAD_UTIL_ROBLOX_USER_CALLBACK_FUNCTIONS_H_
#define CRASHPAD_UTIL_ROBLOX_USER_CALLBACK_FUNCTIONS_H_

namespace crashpad
{
  // called as early as possible after a crash happens
  typedef void(*UserCallbackOnDumpEvent)(void* data);
  void SetUserCallbackOnDumpEvent(UserCallbackOnDumpEvent func);
  void RunUserCallbackOnDumpEvent(void* data);

  // called after crashpad completed minidump generation
  typedef void(*UserCallbackAfterDump)(void* data);
  void SetUserCallbackAfterDump(UserCallbackAfterDump func);
  void RunUserCallbackAfterDump(void* data);
}

#endif
