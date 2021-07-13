#include <limits>
#include "HPCConsensus.h"
#include "MBGCommon.h"
#include "VectorView.h"

// allow multiple threads to update the same contig sequence but in different regions
// each mutex covers MutexLength bp in one contig
// ------
//       ------
//             ------
// etc
// size arbitrarily 1Mbp, so ~(3000 + num_contigs) mutexes in a human genome, hopefully not too many
// and the chance of two random hifis falling in the same 1Mbp bucket is ~.03% so hopefully not too much waiting
constexpr size_t MutexLength = 1000000;

void addCounts(std::vector<CompressedSequenceType>& result, std::vector<std::vector<phmap::flat_hash_map<std::string, size_t>>>& expandedCounts, std::vector<std::vector<std::mutex*>>& seqMutexes, const SequenceCharType& seq, const SequenceLengthType& poses, const std::string& rawSeq, const size_t seqStart, const size_t seqEnd, const size_t unitig, const size_t unitigStart, const size_t unitigEnd, const bool fw)
{
	assert(unitig < expandedCounts.size());
	assert(unitigEnd - unitigStart == seqEnd - seqStart);
	assert(unitigEnd > unitigStart);
	assert(unitigEnd <= expandedCounts[unitig].size());
	size_t lowMutexIndex = unitigStart / MutexLength;
	if (unitigStart > 64) lowMutexIndex = (unitigStart - 64) / MutexLength;
	size_t highMutexIndex = (unitigEnd + 64 + MutexLength - 1) / MutexLength;
	if (highMutexIndex >= seqMutexes[unitig].size()) highMutexIndex = seqMutexes[unitig].size();
	std::vector<std::lock_guard<std::mutex>*> guards;
	for (size_t i = lowMutexIndex; i < highMutexIndex; i++)
	{
		guards.emplace_back(new std::lock_guard<std::mutex>{*seqMutexes[unitig][i]});
	}
	for (size_t i = 0; i < seqEnd - seqStart; i++)
	{
		size_t off = unitigStart + i;
		if (!fw) off = unitigEnd - 1 - i;
		assert(off < result[unitig].compressedSize());
		if (result[unitig].getCompressed(off) == 0)
		{
			if (fw)
			{
				result[unitig].setCompressed(off, seq[seqStart+i]);
			}
			else
			{
				result[unitig].setCompressed(off, complement(seq[seqStart+i]));
			}
		}
		else
		{
			assert(!fw || result[unitig].getCompressed(off) == seq[seqStart + i]);
			assert(fw || result[unitig].getCompressed(off) == complement(seq[seqStart + i]));
		}
		assert(off < expandedCounts[unitig].size());
		assert(seqStart+i+1 < poses.size());
		std::string seq;
		if (fw)
		{
			size_t expandedStart = poses[seqStart+i];
			size_t expandedEnd = poses[seqStart+i+1];
			assert(expandedEnd > expandedStart);
			seq = rawSeq.substr(expandedStart, expandedEnd - expandedStart);
		}
		else
		{
			size_t expandedStart = poses[seqStart+i];
			size_t expandedEnd = poses[seqStart+i+1];
			assert(expandedEnd > expandedStart);
			seq = revCompRaw(rawSeq.substr(expandedStart, expandedEnd - expandedStart));
		}
		expandedCounts[unitig][off][seq] += 1;
	}
	for (size_t i = 0; i < guards.size(); i++)
	{
		delete guards[i];
	}
}

std::vector<CompressedSequenceType> getHPCUnitigSequences(const HashList& hashlist, const UnitigGraph& unitigs, const std::vector<std::string>& filenames, const size_t kmerSize, const ReadpartIterator& partIterator, const size_t numThreads)
{
	std::vector<CompressedSequenceType> result;
	std::vector<std::vector<phmap::flat_hash_map<std::string, size_t>>> expandedCounts;
	result.resize(unitigs.unitigs.size());
	expandedCounts.resize(unitigs.unitigs.size());
	std::vector<std::tuple<size_t, size_t, bool>> kmerPosition;
	kmerPosition.resize(hashlist.size(), std::tuple<size_t, size_t, bool> { std::numeric_limits<size_t>::max(), 0, true });
	std::vector<std::vector<std::mutex*>> seqMutexes;
	seqMutexes.resize(unitigs.unitigs.size());
	size_t rleSize = 0;
	for (size_t i = 0; i < unitigs.unitigs.size(); i++)
	{
		size_t offset = 0;
		for (size_t j = 0; j < unitigs.unitigs[i].size(); j++)
		{
			if (j > 0)
			{
				size_t rleOverlap = hashlist.getOverlap(unitigs.unitigs[i][j-1], unitigs.unitigs[i][j]);
				assert(rleOverlap < kmerSize);
				offset += kmerSize - rleOverlap;
			}
			assert(std::get<0>(kmerPosition[unitigs.unitigs[i][j].first]) == std::numeric_limits<size_t>::max());
			kmerPosition[unitigs.unitigs[i][j].first] = std::make_tuple(i, offset, unitigs.unitigs[i][j].second);
		}
		result[i].resize(offset + kmerSize);
		expandedCounts[i].resize(offset + kmerSize);
		rleSize += result[i].compressedSize();
		for (size_t j = 0; j < offset + kmerSize; j += MutexLength)
		{
			seqMutexes[i].emplace_back(new std::mutex);
		}
	}
	iterateReadsMultithreaded(filenames, numThreads, [&result, &seqMutexes, &expandedCounts, &partIterator, &hashlist, &kmerPosition, kmerSize](size_t thread, FastQ& read)
	{
		partIterator.iteratePartKmers(read, [&result, &seqMutexes, &expandedCounts, &hashlist, &kmerPosition, kmerSize](const SequenceCharType& seq, const SequenceLengthType& poses, const std::string& rawSeq, uint64_t minHash, const std::vector<size_t>& positions)
		{
			size_t currentSeqStart = 0;
			size_t currentSeqEnd = 0;
			size_t currentUnitig = std::numeric_limits<size_t>::max();
			size_t currentUnitigStart = 0;
			size_t currentUnitigEnd = 0;
			size_t currentDiagonal = 0;
			bool currentUnitigForward = true;
			std::vector<std::tuple<size_t, size_t, size_t>> matchBlocks;
			for (auto pos : positions)
			{
				VectorView<CharType> minimizerSequence { seq, pos, pos + kmerSize };
				std::pair<size_t, bool> current;
				current = hashlist.getNodeOrNull(minimizerSequence);
				if (current.first == std::numeric_limits<size_t>::max())
				{
					if (currentUnitig != std::numeric_limits<size_t>::max())
					{
						addCounts(result, expandedCounts, seqMutexes, seq, poses, rawSeq, currentSeqStart, currentSeqEnd, currentUnitig, currentUnitigStart, currentUnitigEnd, currentUnitigForward);
					}
					currentUnitig = std::numeric_limits<size_t>::max();
					continue;
				}
				assert(current.first != std::numeric_limits<size_t>::max());
				assert(current.first < kmerPosition.size());
				assert(std::get<0>(kmerPosition[current.first]) != std::numeric_limits<size_t>::max());
				size_t unitig = std::get<0>(kmerPosition[current.first]);
				size_t offset = std::get<1>(kmerPosition[current.first]);
				bool fw = std::get<2>(kmerPosition[current.first]);
				if (!current.second) fw = !fw;
				size_t diagonal;
				if (fw)
				{
					diagonal = pos - offset;
				}
				else
				{
					diagonal = pos + offset;
				}
				if (unitig == currentUnitig && currentUnitigForward == fw && diagonal == currentDiagonal && pos <= currentSeqEnd)
				{
					assert(pos + kmerSize > currentSeqEnd);
					currentSeqEnd = pos + kmerSize;
					if (fw)
					{
						assert(offset + kmerSize > currentUnitigEnd);
						currentUnitigEnd = offset + kmerSize;
					}
					else
					{
						assert(offset < currentUnitigStart);
						currentUnitigStart = offset;
					}
					continue;
				}
				if (currentUnitig == std::numeric_limits<size_t>::max())
				{
					currentUnitig = unitig;
					currentSeqStart = pos;
					currentSeqEnd = pos + kmerSize;
					currentUnitigStart = offset;
					currentUnitigEnd = offset + kmerSize;
					currentDiagonal = diagonal;
					currentUnitigForward = fw;
					continue;
				}
				addCounts(result, expandedCounts, seqMutexes, seq, poses, rawSeq, currentSeqStart, currentSeqEnd, currentUnitig, currentUnitigStart, currentUnitigEnd, currentUnitigForward);
				currentUnitig = unitig;
				currentSeqStart = pos;
				currentSeqEnd = pos + kmerSize;
				currentUnitigStart = offset;
				currentUnitigEnd = offset + kmerSize;
				currentDiagonal = diagonal;
				currentUnitigForward = fw;
			};
			if (currentUnitig != std::numeric_limits<size_t>::max())
			{
				addCounts(result, expandedCounts, seqMutexes, seq, poses, rawSeq, currentSeqStart, currentSeqEnd, currentUnitig, currentUnitigStart, currentUnitigEnd, currentUnitigForward);
			}
		});
	});
	assert(result.size() == expandedCounts.size());
	for (size_t i = 0; i < expandedCounts.size(); i++)
	{
		assert(result[i].compressedSize() == expandedCounts[i].size());
		for (size_t j = 0; j < expandedCounts[i].size(); j++)
		{
			size_t maxCount = 0;
			std::string maxSeq = "";
			for (auto pair : expandedCounts[i][j])
			{
				if (pair.second <= maxCount) continue;
				maxCount = pair.second;
				maxSeq = pair.first;
			}
			result[i].setExpanded(j, maxSeq);
		}
	}
	for (size_t i = 0; i < seqMutexes.size(); i++)
	{
		for (size_t j = 0; j < seqMutexes[i].size(); j++)
		{
			delete seqMutexes[i][j];
		}
	}
	return result;
}
