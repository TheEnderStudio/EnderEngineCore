#include "EnderVFiles2.hpp"

using namespace std;
using namespace std::filesystem;

EVF_NAMESPACE_BEGIN

Packer& Packer::setAssetDir(const Path& path) {
	if (!exists(path)) {
		EVF_ERROR("ERROR: Path '%s' not exist", path.string().c_str());
		exit(1);
	}
	if (!is_directory(path)) {
		EVF_ERROR("ERROR: Path '%s' is not a directory", path.string().c_str());
		exit(1);
	}
}

EVF_NAMESPACE_END