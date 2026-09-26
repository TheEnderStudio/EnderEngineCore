#include "EnderVFiles2.hpp"

#include <fstream>

using namespace std;
using namespace std::filesystem;

EVF_NAMESPACE_BEGIN

bool IOHelper::createFile(const Path& path) {
	if (is_directory(path)) {
		rename(path, path.string() + '_');
		EVF_INFO("Renamed '%s' to '%s_'", path.string().c_str(), path.string().c_str());
	}
	if (!path.has_parent_path()) {
		create_directory(path.parent_path());
	}
	ofstream fs(path.string().c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
	bool ok = static_cast<bool>(fs);
	fs.close();
	return ok;
}

bool IOHelper::writeFile(const Path& path, const Vector<UInt8>& bytes) {
	if (is_directory(path)) {
		EVF_ERROR("Path '%s' is a directory", path.string().c_str());
		return false;
	}
	if (bytes.size() == 0) {
		return true;
	}
	ofstream fs(path.string().c_str(), std::ios::out | std::ios::app | std::ios::binary);
	if (!fs) {
		EVF_ERROR("Cannot open file '%s'", path.string().c_str());
		return false;
	}
	fs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<streamsize>(bytes.size() * sizeof(bytes[0])));
	fs.close();
	return true;
}

UInt64 IOHelper::readFile(const Path& path, Vector<UInt8>& bytes, UInt64 offset, UInt64 bytesToRead) {
	if (is_directory(path)) {
		EVF_ERROR("Path '%s' is a directory", path.string().c_str());
		return false;
	}
	if (bytesToRead == 0) {
		bytes.clear();
		return true;
	}
	ifstream fs(path.string().c_str(), std::ios::in | std::ios::binary);
	if (!fs) {
		EVF_ERROR("Cannot open file '%s'", path.string().c_str());
		return false;
	}
	streamsize size = fs.tellg();
	fs.seekg(offset, std::ios::beg);
	fs.read(reinterpret_cast<char*>(bytes.data()), static_cast<streamsize>(bytesToRead > size ? size : bytesToRead));
	streamsize result = fs.gcount();
	fs.close();
	return static_cast<UInt64>(result);
}

EVF_NAMESPACE_END