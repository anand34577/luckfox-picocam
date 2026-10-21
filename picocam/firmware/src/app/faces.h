#pragma once
#include <string>
#include <vector>

#include "core/config.h"

// On-device face gallery: people enrolled by showing their face to the camera, matched by cosine similarity of the
// embeddings the AI pipeline computes (embedding models give L2-normalised vectors).
namespace faces {

void load();
json list();  // [{id, name, shots, created}]
std::string remove(const std::string &id);
std::string rename(const std::string &id, const std::string &name);

// Arm enrolment: the next face the camera sees (and keeps seeing) is recorded `shots` times under `name`.
std::string enroll_start(const std::string &name, int shots);
void enroll_cancel();
json enroll_status();  // {active, name, got, shots}

// Called by the AI engine for every embedding of a tracked face. Returns true if it was consumed by an enrolment.
bool offer(const std::vector<float> &embedding, int track_id);

struct Match {
    std::string name;
    float score = 0;
};
// best person above `threshold`; false when nobody matches (or the gallery is empty)
bool match(const std::vector<float> &embedding, float threshold, Match *m);
bool empty();

// similarity helper (exposed for tests)
float cosine(const std::vector<float> &a, const std::vector<float> &b);

}  // namespace faces
