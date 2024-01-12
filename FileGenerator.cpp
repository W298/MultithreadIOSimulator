#include "pch.h"
#include "FileGenerator.h"

using namespace Concurrency::diagnostic;

void FileGenerator::GenerateDummyFiles(const FileGenerationArgs args)
{
	marker_series mainThreadSeries(_T("Main Thread - FileGenerator"));
	span* s = new span(mainThreadSeries, 0, _T("File Generation"));

	UINT* depthFileCountAry = new UINT[args.fileDep.treeDepth];

	UINT truncatedCount = 0;
	UINT largestR = 1;

	// Total file count is too small.
	if (pow(2, args.fileDep.treeDepth) - 1 > args.totalFileCount)
		ExitProcess(-2);

	while (TRUE)
	{
		largestR++;

		truncatedCount = (pow(largestR, args.fileDep.treeDepth) - 1) / (largestR - 1);
		if (truncatedCount > args.totalFileCount)
		{
			largestR--;
			truncatedCount = (pow(largestR, args.fileDep.treeDepth) - 1) / (largestR - 1);
			break;
		}
	}

	UINT rest = args.totalFileCount - truncatedCount;

	for (UINT curDepth = 0; curDepth < args.fileDep.treeDepth; curDepth++)
		depthFileCountAry[curDepth] = 1 * pow(largestR, curDepth);
	
	depthFileCountAry[args.fileDep.treeDepth - 1] += rest;

	CreateDirectoryW(L"dummy", NULL);
	const std::wstring path = L"dummy\\";

	HANDLE* handleAry = new HANDLE[args.totalFileCount];
	BYTE* buffer = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, args.fileSize.maxByte);

	std::random_device rd;
	std::mt19937 generator(rd());
	std::uniform_int_distribution uniformIntDist(0, 1);
	std::normal_distribution<double> sizeNormalDist(args.fileSize.mean, args.fileSize.variance);
	std::normal_distribution<double> computeNormalDist(args.fileCompute.mean, args.fileCompute.variance);

	UINT fidOffset = 0;
	for (UINT curDepth = 0; curDepth < args.fileDep.treeDepth; curDepth++)
	{
		std::vector<std::vector<UINT>> map;
		for (UINT fid = fidOffset; fid < fidOffset + depthFileCountAry[curDepth]; fid++)
		{
			std::cout << fid << " ";

			// File creation.
			UINT64 fileByteSize = max(args.fileSize.minByte, min(args.fileSize.maxByte, round(abs(sizeNormalDist(generator)))));
			const UINT fileComputeTime = max(args.fileCompute.minMicroSeconds, min(args.fileCompute.maxMicroSeconds, round(abs(computeNormalDist(generator)))));
			
			handleAry[fid] = CreateFileW((path + std::to_wstring(fid)).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);

			BYTE* writeAddress = buffer;

			// Write compute time to file.
			memcpy(writeAddress, &fileComputeTime, sizeof(UINT));
			writeAddress += sizeof(UINT);

			if (curDepth < args.fileDep.treeDepth - 1) // Exclude leaf file.
			{
				// File dependency define.
				std::vector<UINT> dependencyVec;
				for (UINT off = 0; off < depthFileCountAry[curDepth + 1]; off++)
				{
					const UINT dependencyFID = fidOffset + depthFileCountAry[curDepth] + off;
					const BOOL fileDependency = args.fileDep.forceAllDep ? 1 : uniformIntDist(generator);

					if (fileDependency != 0)
						dependencyVec.push_back(dependencyFID);
				}

				map.push_back(dependencyVec);
				if (fid == fidOffset + depthFileCountAry[curDepth] - 1)
				{
					for (UINT off = 0; off < depthFileCountAry[curDepth + 1]; off++)
					{
						const UINT dependencyFID = fidOffset + depthFileCountAry[curDepth] + off;
						
						BOOL found = FALSE;
						for (UINT i = 0; i < map.size(); i++)
						{
							for (UINT j = 0; j < map[i].size(); j++)
							{
								if (dependencyFID == map[i][j])
								{
									found = TRUE;
									break;
								}
							}

							if (found)
								break;
						}

						if (FALSE == found)
							dependencyVec.push_back(dependencyFID);
					}
				}

				// Write dependency pair count to file.
				const UINT dependencyPairCount = dependencyVec.size();
				memcpy(writeAddress, &dependencyPairCount, sizeof(UINT));
				writeAddress += sizeof(UINT);

				// If needed file size for writing dependencies exceeds random generated file size, replace it.
				const UINT64 neededFileByteSize = dependencyPairCount * sizeof(UINT) + 2 * sizeof(UINT);
				fileByteSize = max(neededFileByteSize, fileByteSize);

				// Write dependency FID to file.
				for (UINT i = 0; i < dependencyVec.size(); i++)
				{
					const UINT dependencyFID = dependencyVec[i];
					memcpy(writeAddress, &dependencyFID, sizeof(UINT));
					writeAddress += sizeof(UINT);
				}

				WriteFile(handleAry[fid], buffer, fileByteSize, NULL, NULL);
				ZeroMemory(buffer, writeAddress - buffer);
			}
			else
			{
				WriteFile(handleAry[fid], buffer, fileByteSize, NULL, NULL);
				ZeroMemory(buffer, writeAddress - buffer);
			}
		}

		std::cout << "\n";

		fidOffset += depthFileCountAry[curDepth];
	}

	// Release memory.
	for (UINT64 fid = 0; fid < args.totalFileCount; fid++)
		CloseHandle(handleAry[fid]);
	
	HeapFree(GetProcessHeap(), MEM_RELEASE, buffer);
	delete[] handleAry;

	delete s;
}

void FileGenerator::RemoveDummyFiles(UINT64 totalFileCount)
{
	for (UINT fid = 0; fid < totalFileCount; fid++)
	{
		std::wstring path = L"dummy\\";
		DeleteFileW((path + std::to_wstring(fid)).c_str());
	}
	RemoveDirectoryW(L"dummy");
}