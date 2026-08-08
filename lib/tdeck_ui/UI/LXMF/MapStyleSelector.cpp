// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "UI/LXMF/MapStyleSelector.h"

#include <cstring>

namespace Pyxis {

MapStyleSelector::MapStyleSelector()
    : styles_(), count_(0U), active_index_(-1), pending_index_(0U),
      catalog_generation_(0U), next_token_(1U), pending_token_(0U),
      activation_token_(0U), state_(State::DISCOVERING) {}

std::uint32_t MapStyleSelector::advance(std::uint32_t value) {
    ++value;
    return value == 0U ? 1U : value;
}

bool MapStyleSelector::validString(const char* value, std::size_t capacity,
                                   bool identifier) {
    if (value == NULL || capacity < 2U) return false;
    std::size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        const unsigned char character = static_cast<unsigned char>(value[length]);
        if (character < 0x20U || character > 0x7eU) return false;
        if (identifier && !((character >= 'a' && character <= 'z') ||
                            (character >= '0' && character <= '9') ||
                            character == '-' || character == '_')) return false;
        ++length;
    }
    return length > 0U && length < capacity;
}

bool MapStyleSelector::setCatalog(std::uint32_t generation,
                                  const MapStyleSummary* styles,
                                  std::size_t count, const char* active_id) {
    if (activation_token_ != 0U) return false;
    return applyCatalog(generation, styles, count, active_id, State::READY);
}

bool MapStyleSelector::applyCatalog(std::uint32_t generation,
                                    const MapStyleSummary* styles,
                                    std::size_t count, const char* active_id,
                                    State next_state) {
    if (generation == 0U || count > MAX_STYLES ||
        (count != 0U && styles == NULL) ||
        (count == 0U && active_id != NULL)) return false;

    std::ptrdiff_t active = -1;
    bool found_active = active_id == NULL;
    for (std::size_t index = 0U; index < count; ++index) {
        if (!validString(styles[index].id, sizeof(styles[index].id), true) ||
            !validString(styles[index].label, sizeof(styles[index].label), false)) return false;
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (std::strcmp(styles[index].id, styles[previous].id) == 0) return false;
        }
        if (active_id != NULL && std::strcmp(styles[index].id, active_id) == 0) {
            active = static_cast<std::ptrdiff_t>(index);
            found_active = true;
        }
    }
    if (!found_active) return false;

    if (count != 0U) std::memcpy(styles_, styles, count * sizeof(MapStyleSummary));
    count_ = count;
    active_index_ = active;
    pending_index_ = 0U;
    pending_token_ = 0U;
    catalog_generation_ = generation;
    next_token_ = advance(next_token_);
    state_ = next_state;
    return true;
}

bool MapStyleSelector::requestNext(MapStyleRequest& output) {
    if (!canCycle()) return false;
    pending_index_ = active_index_ < 0 ? 0U :
        (static_cast<std::size_t>(active_index_) + 1U) % count_;
    pending_token_ = next_token_;
    next_token_ = advance(next_token_);
    output = MapStyleRequest();
    output.token = pending_token_;
    output.catalog_generation = catalog_generation_;
    std::strcpy(output.style_id, styles_[pending_index_].id);
    activation_token_ = pending_token_;
    state_ = State::APPLYING;
    return true;
}

bool MapStyleSelector::reconcileActivation(
    std::uint32_t expected_token, std::uint32_t generation,
    const MapStyleSummary* styles, std::size_t count, const char* active_id,
    bool retain_error) {
    if (!activationOwnedBy(expected_token)) return false;
    return applyCatalog(generation, styles, count, active_id,
                        retain_error ? State::ERROR : State::READY);
}

bool MapStyleSelector::releaseActivation(std::uint32_t expected_token) {
    if (activation_token_ == 0U || activation_token_ != expected_token) return false;
    activation_token_ = 0U;
    return true;
}

bool MapStyleSelector::cancelPending(std::uint32_t expected_token) {
    if (!activationOwnedBy(expected_token) || state_ != State::APPLYING ||
        pending_token_ != expected_token) return false;
    pending_token_ = 0U;
    activation_token_ = 0U;
    state_ = State::READY;
    return true;
}

bool MapStyleSelector::clearError() {
    if (state_ != State::ERROR) return false;
    state_ = State::READY;
    return true;
}

bool MapStyleSelector::complete(const MapStyleCompletion& completion) {
    if (state_ != State::APPLYING || completion.token != pending_token_ ||
        completion.token != activation_token_ ||
        completion.catalog_generation != catalog_generation_ ||
        std::strcmp(completion.style_id, styles_[pending_index_].id) != 0) return false;
    pending_token_ = 0U;
    if (!completion.success) {
        state_ = State::ERROR;
        return true;
    }
    active_index_ = static_cast<std::ptrdiff_t>(pending_index_);
    state_ = State::READY;
    return true;
}

const char* MapStyleSelector::activeId() const {
    return active_index_ < 0 ? "" : styles_[static_cast<std::size_t>(active_index_)].id;
}

const char* MapStyleSelector::activeLabel() const {
    return active_index_ < 0 ? "Style" :
        styles_[static_cast<std::size_t>(active_index_)].label;
}

}  // namespace Pyxis
