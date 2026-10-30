/*
This file is part of apfs-fuse, a read-only implementation of APFS
(Apple File System) for FUSE.
Copyright (C) 2017 Simon Gander

Apfs-fuse is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

Apfs-fuse is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with apfs-fuse.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef _WIN32

#include "DeviceWinPhys.h"
#include <vector>

DeviceWinPhys::DeviceWinPhys()
{
	m_drive = INVALID_HANDLE_VALUE;
	m_size = 0;
	m_phys_sector_size = 512;
}

DeviceWinPhys::~DeviceWinPhys()
{
	Close();
}

bool DeviceWinPhys::Open(const char *name)
{
	WCHAR path[MAX_PATH];
	uint8_t buf[0x1000];
	DWORD bytes_ret = 0;

	MultiByteToWideChar(CP_UTF8, 0, name, -1, path, MAX_PATH);

	m_drive = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, nullptr);
	if (m_drive == INVALID_HANDLE_VALUE)
		return false;

	if (DeviceIoControl(m_drive, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &buf, sizeof(buf), &bytes_ret, nullptr) && bytes_ret > 0)
	{
		const DISK_GEOMETRY_EX *geom = reinterpret_cast<const DISK_GEOMETRY_EX *>(buf);
		m_size = geom->DiskSize.QuadPart;
		m_phys_sector_size = geom->Geometry.BytesPerSector ? geom->Geometry.BytesPerSector : 512;
	}
	else
	{
		GET_LENGTH_INFORMATION gli = {};
		if (DeviceIoControl(m_drive, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &gli, sizeof(gli), &bytes_ret, nullptr))
		{
			m_size = gli.Length.QuadPart;
		}
		DISK_GEOMETRY dg = {};
		if (DeviceIoControl(m_drive, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &dg, sizeof(dg), &bytes_ret, nullptr))
		{
			m_phys_sector_size = dg.BytesPerSector ? dg.BytesPerSector : 512;
		}
	}

	if (m_phys_sector_size == 0)
		m_phys_sector_size = 512;

	return (m_size > 0);
}

void DeviceWinPhys::Close()
{
	if (m_drive != INVALID_HANDLE_VALUE)
		CloseHandle(m_drive);
	m_drive = INVALID_HANDLE_VALUE;
	m_size = 0;
}

bool DeviceWinPhys::Read(void * data, uint64_t offs, uint64_t len)
{
	if (m_drive == INVALID_HANDLE_VALUE || len == 0)
		return false;

	uint32_t sec = m_phys_sector_size ? m_phys_sector_size : 512;

	// Check if already sector aligned
	if ((offs % sec == 0) && (len % sec == 0))
	{
		LARGE_INTEGER off;
		off.QuadPart = offs;
		if (!SetFilePointerEx(m_drive, off, nullptr, FILE_BEGIN))
			return false;

		DWORD read_bytes = 0;
		if (!ReadFile(m_drive, data, (DWORD)len, &read_bytes, nullptr) || read_bytes != len)
			return false;

		return true;
	}

	// Handle unaligned read with sector bounce buffer
	uint64_t aligned_offs = (offs / sec) * sec;
	uint64_t aligned_end = ((offs + len + sec - 1) / sec) * sec;
	size_t aligned_len = (size_t)(aligned_end - aligned_offs);

	std::vector<uint8_t> bounce(aligned_len);

	LARGE_INTEGER off;
	off.QuadPart = aligned_offs;
	if (!SetFilePointerEx(m_drive, off, nullptr, FILE_BEGIN))
		return false;

	DWORD read_bytes = 0;
	if (!ReadFile(m_drive, bounce.data(), (DWORD)aligned_len, &read_bytes, nullptr))
		return false;

	if (read_bytes < (offs - aligned_offs + len))
		return false;

	memcpy(data, bounce.data() + (offs - aligned_offs), (size_t)len);
	return true;
}

uint64_t DeviceWinPhys::GetSize() const
{
	return m_size;
}

#endif
