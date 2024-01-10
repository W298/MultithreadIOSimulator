#pragma once

// #define PRINT_FILE_GEN

namespace FileGenerator
{
	enum FileDependencyType
	{
		FILE_DEPENDENCY_NONE,
		FILE_DEPENDENCY_ONLY_READ,
		FILE_DEPENDENCY_NEED_COMPUTE
	};

	UINT64 GenerateDummyFiles(
		const UINT depth, const UINT* fileCountAry, const UINT64 minByte, const UINT64 maxByte, 
		const UINT mean, const UINT variance);
	void RemoveDummyFiles();
}
