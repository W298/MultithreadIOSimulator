#pragma once

namespace FileGenerator
{
	enum FileDependencyModel
	{
		FILE_DEPENDENCY_PYRAMID,
		FILE_DEPENDENCY_REVERSE_PYRAMID,
		FILE_DEPENDENCY_UNIFORM
	};

	struct FileSizeArgs
	{
		UINT64 minByte;
		UINT64 maxByte;
		UINT mean;
		UINT variance;
	};

	struct FileComputeArgs
	{
		UINT minMicroSeconds;
		UINT maxMicroSeconds;
		UINT mean;
		UINT variance;
	};

	struct FileDependencyArgs
	{
		FileDependencyModel model;
		UINT treeDepth;
		BOOL forceAllDep;
	};
	
	struct FileGenerationArgs
	{
		UINT64 totalFileCount;
		FileSizeArgs fileSize;
		FileComputeArgs fileCompute;
		FileDependencyArgs fileDep;
	};

	void GenerateDummyFiles(const FileGenerationArgs fileGenerationArgs);
	void RemoveDummyFiles(UINT64 totalFileCount);
}
