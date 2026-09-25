#include "EnderVFiles2.hpp"

EVF_NAMESPACE_BEGIN

#define Stringfy_(x) #x
#define Stringfy(x) Stringfy_(x)

static const char* OPENSSL_VERSION = Stringfy(
#include <openssl/version.inc>
);

const char* VFS::getOpenSSLVersion() {
	return OPENSSL_VERSION;
}

EVF_NAMESPACE_END