#pragma once
// Pure (host-testable, no Arduino) parsers for the dashboard config messages.
#include <stddef.h>
#include <stdint.h>

#define CONTACT_NAME_CHARS 24  // names truncated to 24 characters (UTF-8 aware)
#define CONTACT_NAME_LEN  (CONTACT_NAME_CHARS * 3 + 1)   // bytes incl. NUL (Bangla = 3 B/char)
#define CONTACT_PHONE_LEN 20
#define CONTACTS_MAX_PARSE 16

struct Contact { char name[CONTACT_NAME_LEN]; char phone[CONTACT_PHONE_LEN]; };

enum class CfgResult { Ok, NotForUs, Error };

struct ContactsMsg {
  uint64_t version;
  size_t count;              // valid contacts stored in `contacts`
  size_t rejected;           // entries skipped (bad phone / bad entry)
  Contact contacts[CONTACTS_MAX_PARSE];
  const char* error;         // set when result == Error
};

struct SmsCfgMsg {
  uint64_t version;
  bool smsEnabled;
  const char* error;
};

// contacts entries may be {"name":"..","phone":"+880.."} objects or plain "+880.." strings
CfgResult parseContactsMsg(const uint8_t* json, size_t len, const char* deviceId,
                           const char* token, uint64_t currentVersion, size_t maxContacts,
                           ContactsMsg& out);
CfgResult parseSmsCfgMsg(const uint8_t* json, size_t len, const char* deviceId,
                         const char* token, uint64_t currentVersion, SmsCfgMsg& out);
bool validPhone(const char* s);
