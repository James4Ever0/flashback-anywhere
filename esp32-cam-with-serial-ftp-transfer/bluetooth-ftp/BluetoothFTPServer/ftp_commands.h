#ifndef FTP_COMMANDS_H
#define FTP_COMMANDS_H

#include <stddef.h>  // for size_t

// Set the expected application-level PIN (read from /btpin.txt in setup()).
void ftpSetExpectedPin(const char* pin);

// Set authenticated state (true after successful AUTH, false on disconnect).
void ftpSetAuthenticated(bool state);

// Parse a received command line and dispatch to the appropriate handler.
// This function is called from within taskBluetoothFTP.
void ftpDispatch(const char* line);

// Individual command handlers (called by ftpDispatch)
void cmdAuth(const char* pin);
void cmdHelp(void);
void cmdList(const char* path);
void cmdRead(const char* path);
void cmdStore(const char* path, size_t size);
void cmdDelete(const char* path);
void cmdSetPin(const char* pin);
void cmdBtInfo(void);

#endif
