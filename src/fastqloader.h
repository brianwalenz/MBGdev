#ifndef FastqLoader_H
#define FastqLoader_H

#include <string>
#include <vector>
#include <zstr.hpp> //https://github.com/mateidavid/zstr

class FastQ {
public:
	template <typename F>
	static void streamFastqFastqFromStream(std::istream& file, bool includeQuality, F f)
	{
		do
		{
			std::string line;
			std::getline(file, line);
			if (line.size() == 0) continue;
			if (line[0] != '@') continue;
			FastQ newread;
			if (line.back() == '\r') line.pop_back();
			newread.seq_id = line.substr(1);
			std::getline(file, line);
			if (line.back() == '\r') line.pop_back();
			// uppercase
			for (size_t i = 0; i < line.size(); i++)
			{
				if (line[i] >= 'a' && line[i] <= 'z') line[i] = 'A' + (line[i] - 'a');
			}
			newread.sequence = line;
			std::getline(file, line);
			std::getline(file, line);
			if (line.back() == '\r') line.pop_back();
			if (includeQuality) newread.quality = line;
			f(newread);
		} while (file.good());
	}
	template <typename F>
	static void streamFastqFastaFromStream(std::istream& file, bool includeQuality, F f)
	{
		std::string line;
		std::getline(file, line);
		do
		{
			if (line.size() == 0)
			{
				std::getline(file, line);
				continue;
			}
			if (line[0] != '>')
			{
				std::getline(file, line);
				continue;
			}
			FastQ newread;
			if (line.back() == '\r') line.pop_back();
			newread.seq_id = line.substr(1);
			newread.sequence = "";
			do
			{
				std::getline(file, line);
				if (line.size() == 0) continue;
				if (line[0] == '>') break;
				if (line.back() == '\r') line.pop_back();
				// uppercase
				for (size_t i = 0; i < line.size(); i++)
				{
					if (line[i] >= 'a' && line[i] <= 'z') line[i] = 'A' + (line[i] - 'a');
				}
				newread.sequence += line;
			} while (file.good());
			if (includeQuality)
			{
				for (size_t i = 0; i < newread.sequence.size(); i++)
				{
					newread.quality += '!';
				}
			}
			f(newread);
		} while (file.good());
	}
	template <typename F>
	static void streamFastqFastqFromFile(std::string filename, bool includeQuality, F f)
	{
		std::ifstream file {filename};
		streamFastqFastqFromStream(file, includeQuality, f);
	}
	template <typename F>
	static void streamFastqFastaFromFile(std::string filename, bool includeQuality, F f)
	{
		std::ifstream file {filename};
		streamFastqFastaFromStream(file, includeQuality, f);
	}
	template <typename F>
	static void streamFastqFastqFromGzippedFile(std::string filename, bool includeQuality, F f)
	{
		zstr::ifstream file { filename };
		streamFastqFastqFromStream(file, includeQuality, f);
	}
	template <typename F>
	static void streamFastqFastaFromGzippedFile(std::string filename, bool includeQuality, F f)
	{
		zstr::ifstream file { filename };
		streamFastqFastaFromStream(file, includeQuality, f);
	}
	template <typename F>
	static void streamFastqFromFile(std::string filename, bool includeQuality, F f)
	{
		bool gzipped = false;
		std::string originalFilename = filename;
		if (filename.substr(filename.size()-3) == ".gz")
		{
			gzipped = true;
			filename = filename.substr(0, filename.size()-3);
		}
		bool fastq = false;
		bool fasta = false;
		bool stdin = false;
		if ((filename.size() > 6) && (filename.substr(filename.size()-6) == ".fastq")) fastq = true;
		if ((filename.size() > 3) && (filename.substr(filename.size()-3)) == ".fq") fastq = true;
		if ((filename.size() > 6) && (filename.substr(filename.size()-6)) == ".fasta") fasta = true;
		if ((filename.size() > 3) && (filename.substr(filename.size()-3)) == ".fa") fasta = true;
		if (filename == "-.fastq") fastq = stdin = true;
		if (filename == "-.fq") fastq = stdin = true; 
		if (filename == "-.fasta") fasta = stdin = true;
		if (filename == "-.fa") fasta = stdin = true;
		if (fasta)
		{
			if (stdin)
			{
				streamFastqFastaFromStream(std::cin, includeQuality, f);
			}
			else if (gzipped)
			{
				streamFastqFastaFromGzippedFile(originalFilename, includeQuality, f);
				return;
			}
			else
			{
				streamFastqFastaFromFile(originalFilename, includeQuality, f);
				return;
			}
		}
		if (fastq)
		{
			if (stdin)
			{
				streamFastqFastqFromStream(std::cin, includeQuality, f);
			}
			else if (gzipped)
			{
				streamFastqFastqFromGzippedFile(originalFilename, includeQuality, f);
				return;
			}
			else
			{
				streamFastqFastqFromFile(originalFilename, includeQuality, f);
				return;
			}
		}
	}
	FastQ reverseComplement() const;
	std::string seq_id;
	std::string sequence;
	std::string quality;
};

std::vector<FastQ> loadFastqFromFile(std::string filename, bool includeQuality = true);

#endif
