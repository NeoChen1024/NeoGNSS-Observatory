// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <mutex>
namespace neognss_obs {
// RTKLIB has shared caches: all project adapters use this same lock.
inline std::mutex rtklib_mutex;
} // namespace neognss_obs
