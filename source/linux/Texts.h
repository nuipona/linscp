/*
 * Texts: user visible strings of the front end.
 *
 * Upstream keeps its strings in resource files so that they can be
 * translated; the console client keeps the same discipline with one
 * module holding every message, in English.
 */

#ifndef LINUXSCP_TEXTS_H
#define LINUXSCP_TEXTS_H

#include <cstdint>
#include <string>

namespace linuxscp {
namespace Texts {

extern const char * const ProductName;
extern const char * const ProductVersion;

std::string VersionLine();
std::string Usage();

std::string CannotOpenSession(const std::string & host, const std::string & reason);
std::string ConnectionClosed();
std::string HostKeyUnknown(const std::string & host, int port,
    const std::string & keyType, const std::string & fingerprint);
std::string HostKeyMismatch(const std::string & host, int port,
    const std::string & keyType, const std::string & fingerprint);
std::string HostKeyUnexpected(const std::string & host, int port,
    const std::string & keyType, const std::string & fingerprint,
    const std::string & expected);
std::string HostKeyCached(const std::string & host, int port);
std::string PasswordPrompt(const std::string & userName, const std::string & host);
std::string NotConnected();
std::string ReadingDirectory(const std::string & path);
std::string DirectoryRead(int count);
std::string CannotReadDirectory(const std::string & path, const std::string & reason);
std::string RetrievingFile(const std::string & remote, const std::string & local);
std::string UploadingFile(const std::string & local, const std::string & remote);
std::string FileTransferred(const std::string & name, uint64_t size);
std::string TransferFailed(const std::string & name, const std::string & reason);
std::string RemoteError(const std::string & operation, const std::string & path,
    const std::string & reason);
std::string LocalError(const std::string & operation, const std::string & path,
    const std::string & reason);
std::string UnknownCommand(const std::string & command);
std::string WrongArguments(const std::string & command, const std::string & hint);

} // namespace Texts
} // namespace linuxscp

#endif /* LINUXSCP_TEXTS_H */
