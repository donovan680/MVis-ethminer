
/*
This file is part of mvis-ethereum.

mvis-ethereum is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

mvis-ethereum is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with mvis-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Misc.h"
#include <algorithm>

#ifdef _WIN32
#include <Shlobj.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#endif

using namespace std;
using namespace boost;

// read one line of text from the input stream. this handles mixed types of line endings.
bool getlineEx(std::istream& _is, std::string& _str)
{
	_str.clear();

	// The characters in the stream are read one-by-one using a std::streambuf.
	// Code that uses streambuf this way must be guarded by a sentry object.
	// The sentry object performs various tasks, such as thread synchronization 
	// and updating the stream state.

	if (_is.eof())
		return false;

	try
	{
		std::istream::sentry se(_is, true);
		std::streambuf* sb = _is.rdbuf();

		for (;;)
		{
			int c = sb->sbumpc();
			switch (c)
			{
				case '\n':
					return true;
				case '\r':
					if (sb->sgetc() == '\n')
						sb->sbumpc();
					return true;
				case EOF:
					_is.setstate(std::ios::eofbit);
					return true;
				default:
					_str += (char) c;
			}
		}
	}
	catch (std::exception& e)
	{
		std::cerr << "getlineEx exception : " << e.what() << std::endl;
		return false;
	}

}


// get the path to the app data folder, which is %LocalAppData%\ethminer on Windows, and HOME/.config/ethminer elsewhere. 
// create necessary folder elements if they don't exist.
filesystem::path getAppDataFolder(void)
{
#ifdef _WIN32
	PWSTR pszPath;

	// get %AppData%/Local
	HRESULT res = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &pszPath);
	std::wstring local(pszPath);
	CoTaskMemFree(pszPath);

	filesystem::path s("ethminer");
#else
	const char *local;
	if ((local = getenv("HOME")) == NULL)
	{
		local = getpwuid(getuid())->pw_dir;
	}
	filesystem::path s(".config/ethminer");
#endif
	s = local / s;
	if (!filesystem::exists(s))
		filesystem::create_directory(s);

	return s;
}

std::string& LowerCase(std::string& _s)
{
	// this does an in-place transformation
	std::transform(_s.begin(), _s.end(), _s.begin(), ::tolower);
	return _s;
}

bool fileExists(std::string _path)
{
	std::ifstream file(_path);
	return file.good();
}
