#pragma once
#include <string>
#include <utility>
#include <vector>

// Plate / text reading helpers (pure logic).
//
// A plate read once is often wrong in one character; read ten times it is usually right. Voting on whole strings
// throws away what the near-misses agree on, so the consensus is built per character position among the reads that have
// the most likely length.
std::string text_consensus(const std::vector<std::pair<std::string, float>> &reads, float *weight, int *support);

// Edit distance for matching a reading against a watch-list: look-alike characters (0/O/D/Q, 1/I/L, 8/B, 5/S, 2/Z, 6/G,
// 4/A, 7/T) cost 0.3 instead of 1, so one misread character does not defeat the list.
float text_distance(const std::string &a, const std::string &b);
