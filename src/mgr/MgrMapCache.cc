// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2026 IBM
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "mgr/MgrMapCache.h"

#include <algorithm>
#include <optional>

#include "common/config_proxy.h"
#include "common/debug.h"
#include "global/global_context.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mgr
#undef dout_prefix
#define dout_prefix *_dout << "api cache " << __func__ << " "

template<class Key, class Value>
MgrMapCache<Key,Value>::~MgrMapCache() {
  g_conf().remove_observer(this);
  this->clear();
}

template<class Key, class Value>
bool LFUCache<Key,Value>::extract(std::string_view k, Value* out) noexcept {
  std::unique_lock<std::shared_mutex> l(m);
  auto it = cache_data.find(std::string(k));
  if (it == cache_data.end()) return false;
  *out = it->second.val;
  cache_data.erase(it);
  return true;
}

template<class Key, class Value>
void LFUCache<Key,Value>::drain(std::vector<Value>& out) noexcept {
  std::unique_lock<std::shared_mutex> l(m);
  out.reserve(cache_data.size());
  for (auto& kv : cache_data) out.push_back(kv.second.val);
  cache_data.clear();
  hits.store(0);
  misses.store(0);
}

template<class Key, class Value>
Value LFUCache<Key,Value>::get(std::string_view k) {
    std::unique_lock l(m);
    if (!is_enabled()) throw std::out_of_range("cache disabled");
    auto it = cache_data.find(std::string(k));
    if (it == cache_data.end()){
      throw std::out_of_range(std::string(k));
    }
    it->second.hits++;
    mark_hit();
    return it->second.val;
  }

template<class Key, class Value>
bool LFUCache<Key,Value>::try_get(std::string_view k, Value* out, bool count_hit) noexcept {
  std::unique_lock l(m);
  auto it = cache_data.find(std::string(k));
  if (it==cache_data.end()) {
    mark_miss();
    return false;
  }
  if (count_hit) {
    it->second.hits++;
    mark_hit();
  }
  *out = it->second.val;
  return true;
}

template<class Key, class Value>
std::optional<Value> LFUCache<Key,Value>::insert(std::string_view  key, Value value) {
  if (!can_write_cache(key)) {
    return std::nullopt;
  }

  std::unique_lock<std::shared_mutex> l(m);
  std::string key_str(key);
  auto it = cache_data.find(key_str);
  if (it != cache_data.end()) {
    // If key exists, return old value for cleanup
    Value old_val = it->second.val;
    it->second.val = std::move(value);
    return old_val;
  }
  
  // New insert counts as a miss (cache didn't have it)
  mark_miss();

  // at capacity? evict the lowest‐hit key
  std::optional<Value> evicted;
  if (cache_data.size() >= capacity) {
    auto min_it = std::min_element(cache_data.begin(),
      cache_data.end(),
      [](const auto& a, const auto& b) {
        return a.second.hits.load(std::memory_order_relaxed) <
               b.second.hits.load(std::memory_order_relaxed);
    });
    evicted = min_it->second.val;
    cache_data.erase(min_it);
  }

  cache_data.emplace(std::move(key_str), Entry(std::move(value)));
  return evicted;
}

template <class Key, class Value>
MgrMapCache<Key, Value>::MgrMapCache(uint16_t size)
    : CacheImp(size, g_conf().get_val<bool>("mgr_map_cache_enabled")) {
  dout(20) << ": creating cache with size " << size << dendl;
  g_conf().add_observer(this);
}

template <class Key, class Value>
void MgrMapCache<Key, Value>::handle_conf_change(
    const ConfigProxy& conf, const std::set<std::string>& changed) {
    if (changed.count("mgr_map_cache_enabled")) {
      this->set_enabled(conf.get_val<bool>("mgr_map_cache_enabled"));
    }
}

template <class Key>
MgrMapCache<Key, PyObject*>::MgrMapCache(uint16_t size)
    : CacheImp(size, g_conf().get_val<bool>("mgr_map_cache_enabled")) {
    dout(20) << ": creating cache with size " << size << dendl;
    g_conf().add_observer(this);
}

template <class Key>
MgrMapCache<Key, PyObject*>::~MgrMapCache() {
  g_conf().remove_observer(this);
  CacheImp::clear();
}


template <class Key>
PyObject* MgrMapCache<Key, PyObject*>::get(std::string_view k) {
  if (!this->is_enabled() || !this->is_cacheable(k)) return nullptr;
  ceph_assert(PyGILState_Check());
  PyObject* o=nullptr;
  if (!CacheImp::try_get(k,&o,true)) return nullptr;
  Py_INCREF(o);                 // hand out a new ref
  return o;
}

template <class Key>
void MgrMapCache<Key, PyObject*>::insert(std::string_view key, PyObject* value) {
  if (!this->can_write_cache(key)) {
    return;
  }
  ceph_assert(PyGILState_Check());

  Py_INCREF(value); //Inc before insert
  auto evicted = CacheImp::insert(key, value);

  if (evicted.has_value()) {
    PyObject* old_obj = evicted.value();
    if (Py_AddPendingCall(+[](void* p){ Py_DECREF((PyObject*)p); return 0; }, old_obj) != 0) {
      Py_DECREF(old_obj);
    }
  }

  dout(20) << ": inserted key: " << key << " py count: "
           << Py_REFCNT(value) << " hit/miss:"
           << CacheImp::get_hits() << "/"
           << CacheImp::get_misses() << dendl;
}

template <class Key>
void MgrMapCache<Key, PyObject*>::erase(std::string_view key) noexcept {
  if (!this->is_cacheable(key)) return;
  PyObject* o = nullptr;
  if (!this->extract(key, &o)) return;
  if (Py_AddPendingCall(+[](void* p){ Py_DECREF((PyObject*)p); return 0; }, o) != 0) {
    PyGILState_STATE st = PyGILState_Ensure();
    Py_DECREF(o);
    PyGILState_Release(st);
  }
  dout(20) << ": erased key: " << key << " py count: "
         << (o ? Py_REFCNT(o) : 0)
         << " hit/miss:"
         << CacheImp::get_hits() << "/"
         << CacheImp::get_misses() << dendl;
}

template <class Key>
void MgrMapCache<Key, PyObject*>::clear() noexcept {
  std::vector<PyObject*> to_drop;
  this->drain(to_drop);
  if (to_drop.empty()) return;
  PyGILState_STATE st = PyGILState_Ensure();
  for (auto* o : to_drop) Py_DECREF(o);
  PyGILState_Release(st);
  dout(20) << ": Cache cleared" << dendl;
}

template <class Key>
void MgrMapCache<Key, PyObject*>::handle_conf_change(
    const ConfigProxy& conf, const std::set<std::string>& changed) {
    if (changed.count("mgr_map_cache_enabled")) {
      this->set_enabled(conf.get_val<bool>("mgr_map_cache_enabled"));
    }
}


template class MgrMapCache<std::string, PyObject*>;

// Explicit instantiation for unit tests
template class LFUCache<std::string, int>;