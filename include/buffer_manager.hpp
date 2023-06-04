// Copyright (c) 2020-2021 Gregor Daiß
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BUFFER_MANAGER_HPP
#define BUFFER_MANAGER_HPP

#include <cassert>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#ifdef CPPUDDLE_HAVE_HPX  
// For builds with The HPX mutex
#include <hpx/mutex.hpp>
#endif

#ifdef CPPUDDLE_HAVE_COUNTERS
#include <boost/core/demangle.hpp>
#endif

// TODO Switch mutex_t globally?
// -- Problem: Mutex only works when HPX is initialized (what about non HPX
// builds?)
// -- What about the times when HPX is not initialized anymore (locks in
// destructor + static)
// TODO add mutex type to template parameter list?
// -- What about adding stuff to other parts of the code? mutex for callbacks?
//
// Decision:
// Switch globally
// Otherwise the template mutex parameter leaks into the interface of the
// allocators which complicates both the code and the usage. It also generates
// corner cases of intermixing mutexes...

namespace recycler {
constexpr size_t number_instances = 128;
namespace detail {

#ifdef CPPUDDLE_HAVE_HPX  
using mutex_t = hpx::lcos::local::mutex;
#else
using mutex_t = std::mutex;
#endif

class buffer_recycler {
  // Public interface
public:
  /// Returns and allocated buffer of the requested size - this may be a reused
  /// buffer
  template <typename T, typename Host_Allocator>
  static T *get(size_t number_elements, bool manage_content_lifetime = false,
      std::optional<size_t> location_hint = std::nullopt) {
    return buffer_manager<T, Host_Allocator>::get(number_elements,
                                                  manage_content_lifetime, location_hint);
  }
  /// Marks an buffer as unused and fit for reusage
  template <typename T, typename Host_Allocator>
  static void mark_unused(T *p, size_t number_elements,
      std::optional<size_t> location_hint = std::nullopt) {
    return buffer_manager<T, Host_Allocator>::mark_unused(p, number_elements);
  }
  /// Deallocate all buffers, no matter whether they are marked as used or not
  static void clean_all() {
    std::lock_guard<mutex_t> guard(instance().callback_protection_mut);
    for (const auto &clean_function :
         instance().total_cleanup_callbacks) {
      clean_function();
    }
  }
  /// Deallocated all currently unused buffer
  static void clean_unused_buffers() {
    std::lock_guard<mutex_t> guard(instance().callback_protection_mut);
    for (const auto &clean_function :
         instance().partial_cleanup_callbacks) {
      clean_function();
    }
  }

  // Member variables and methods
private:

  /// Singleton instance access
  static buffer_recycler& instance() {
    static buffer_recycler singleton{};
    return singleton;
  }
  /// Callbacks for buffer_manager cleanups - each callback completely destroys
  /// one buffer_manager
  std::list<std::function<void()>> total_cleanup_callbacks;
  /// Callbacks for partial buffer_manager cleanups - each callback deallocates
  /// all unused buffers of a manager
  std::list<std::function<void()>> partial_cleanup_callbacks;
  /// default, private constructor - not automatically constructed due to the
  /// deleted constructors
  buffer_recycler() = default;

  mutex_t callback_protection_mut;
  /// Add a callback function that gets executed upon cleanup and destruction
  static void add_total_cleanup_callback(const std::function<void()> &func) {
    std::lock_guard<mutex_t> guard(instance().callback_protection_mut);
    instance().total_cleanup_callbacks.push_back(func);
  }
  /// Add a callback function that gets executed upon partial (unused memory)
  /// cleanup
  static void add_partial_cleanup_callback(const std::function<void()> &func) {
    std::lock_guard<mutex_t> guard(instance().callback_protection_mut);
    instance().partial_cleanup_callbacks.push_back(func);
  }

public:
  ~buffer_recycler() = default; // public destructor for unique_ptr instance

  // Subclasses
private:
  /// Memory Manager subclass to handle buffers a specific type
  template <typename T, typename Host_Allocator> class buffer_manager {
  private:
    // Tuple content: Pointer to buffer, buffer_size, location ID, Flag
    // The flag at the end controls whether to buffer content is to be reused as
    // well
    using buffer_entry_type = std::tuple<T *, size_t, size_t, bool>;

  public:
    /// Cleanup and delete this singleton
    static void clean() {
      instance().reset(new buffer_manager[number_instances]);
    }
    /// Cleanup all buffers not currently in use
    static void clean_unused_buffers_only() {
      for (auto i = 0; i < number_instances; i++) {
        std::lock_guard<mutex_t> guard(instance()[i].mut);
        for (auto &buffer_tuple : instance()[i].unused_buffer_list) {
          Host_Allocator alloc;
          if (std::get<3>(buffer_tuple)) {
            std::destroy_n(std::get<0>(buffer_tuple), std::get<1>(buffer_tuple));
          }
          alloc.deallocate(std::get<0>(buffer_tuple), std::get<1>(buffer_tuple));
        }
        instance()[i].unused_buffer_list.clear();
      }
    }

    /// Tries to recycle or create a buffer of type T and size number_elements.
    static T *get(size_t number_of_elements, bool manage_content_lifetime,
        std::optional<size_t> location_hint = std::nullopt) {
      init_callbacks_once();

      size_t location_id = 0;
      if (location_hint) {
        location_id = location_hint.value();
      }
      std::lock_guard<mutex_t> guard(instance()[location_id].mut);


#ifdef CPPUDDLE_HAVE_COUNTERS
      instance()[location_id].number_allocation++;
#endif
      // Check for unused buffers we can recycle:
      for (auto iter = instance()[location_id].unused_buffer_list.begin();
           iter != instance()[location_id].unused_buffer_list.end(); iter++) {
        auto tuple = *iter;
        if (std::get<1>(tuple) == number_of_elements) {
          instance()[location_id].unused_buffer_list.erase(iter);

          // handle the switch from aggressive to non aggressive reusage (or
          // vice-versa)
          if (manage_content_lifetime && !std::get<3>(tuple)) {
            std::uninitialized_value_construct_n(std::get<0>(tuple),
                                                  number_of_elements);
            std::get<3>(tuple) = true;
          } else if (!manage_content_lifetime && std::get<3>(tuple)) {
            std::destroy_n(std::get<0>(tuple), std::get<1>(tuple));
            std::get<3>(tuple) = false;
          }
          instance()[location_id].buffer_map.insert({std::get<0>(tuple), tuple});
#ifdef CPPUDDLE_HAVE_COUNTERS
          instance()[location_id].number_recycling++;
#endif
          return std::get<0>(tuple);
        }
      }

      // No unused buffer found -> Create new one and return it
      try {
        Host_Allocator alloc;
        T *buffer = alloc.allocate(number_of_elements);
        instance()[location_id].buffer_map.insert(
            {buffer, std::make_tuple(buffer, number_of_elements, 1,
                                     manage_content_lifetime)});
#ifdef CPPUDDLE_HAVE_COUNTERS
        instance()[location_id].number_creation++;
#endif
        if (manage_content_lifetime) {
          std::uninitialized_value_construct_n(buffer, number_of_elements);
        }
        return buffer;
      } catch (std::bad_alloc &e) {
        // not enough memory left! Cleanup and attempt again:
        std::cerr << "Not enough memory left. Cleaning up unused buffers now..." << std::endl;
        buffer_recycler::clean_unused_buffers();

        // If there still isn't enough memory left, the caller has to handle it
        // We've done all we can in here
        Host_Allocator alloc;
        T *buffer = alloc.allocate(number_of_elements);
        instance()[location_id].buffer_map.insert(
            {buffer, std::make_tuple(buffer, number_of_elements, 1,
                                     manage_content_lifetime)});
#ifdef CPPUDDLE_HAVE_COUNTERS
        instance()[location_id].number_creation++;
        instance()[location_id].number_bad_alloc++;
#endif
        if (manage_content_lifetime) {
          std::uninitialized_value_construct_n(buffer, number_of_elements);
        }
        return buffer;
      }
    }

    static void mark_unused(T *memory_location, size_t number_of_elements,
        std::optional<size_t> location_hint = std::nullopt) {

      if (location_hint) {
        size_t location_id = location_hint.value();
        std::lock_guard<mutex_t> guard(instance()[location_id].mut);
        if (instance()[location_id].buffer_map.find(memory_location) !=
            instance()[location_id].buffer_map.end()) {
#ifdef CPPUDDLE_HAVE_COUNTERS
          instance()[location_id].number_dealloacation++;
#endif
          auto it = instance()[location_id].buffer_map.find(memory_location);
          assert(it != instance()[location_id].buffer_map.end());
          auto &tuple = it->second;
          // sanity checks:
          assert(std::get<1>(tuple) == number_of_elements);
          // move to the unused_buffer list
          instance()[location_id].unused_buffer_list.push_front(tuple);
          instance()[location_id].buffer_map.erase(memory_location);
          return; // Success
        }
        // hint was wrong - note that, and continue on with all other buffer
        // managers
#ifdef CPPUDDLE_HAVE_COUNTERS
        instance()[location_id].number_wrong_hints++;
#endif
      }

      for(size_t location_id = 0; location_id < number_instances; location_id++) {
        if (location_hint) {
           if (location_hint.value() == location_id) {
             continue; // already tried this -> skip
           }
        }
        std::lock_guard<mutex_t> guard(instance()[location_id].mut);
        if (instance()[location_id].buffer_map.find(memory_location) !=
            instance()[location_id].buffer_map.end()) {
#ifdef CPPUDDLE_HAVE_COUNTERS
          instance()[location_id].number_dealloacation++;
#endif
          auto it = instance()[location_id].buffer_map.find(memory_location);
          assert(it != instance()[location_id].buffer_map.end());
          auto &tuple = it->second;
          // sanity checks:
          assert(std::get<1>(tuple) == number_of_elements);
          // move to the unused_buffer list
          instance()[location_id].unused_buffer_list.push_front(tuple);
          instance()[location_id].buffer_map.erase(memory_location);
          return; // Success
        }
      }

      // Failure -- something is very wrong
      throw std::runtime_error("Tried to delete non-existing buffer");
    }

  private:
    /// List with all buffers still in usage
    std::unordered_map<T *, buffer_entry_type> buffer_map{};
    /// List with all buffers currently not used
    std::list<buffer_entry_type> unused_buffer_list{};
    /// Access control
    mutex_t mut;
#ifdef CPPUDDLE_HAVE_COUNTERS
    /// Performance counters
    size_t number_allocation{0}, number_dealloacation{0}, number_wrong_hints{0};
    size_t number_recycling{0}, number_creation{0}, number_bad_alloc{0};
#endif
    /// Singleton instance
    /* static std::unique_ptr<buffer_manager<T, Host_Allocator>> manager_instance; */
    /// default, private constructor - not automatically constructed due to the
    /// deleted constructors
    buffer_manager() = default;
    buffer_manager<T, Host_Allocator>&
    operator=(buffer_manager<T, Host_Allocator> const &other) = default;
    buffer_manager<T, Host_Allocator>&
    operator=(buffer_manager<T, Host_Allocator> &&other) = delete;
    static std::unique_ptr<buffer_manager[]>& instance(void) {
      /* static std::array<buffer_manager, number_instances> instances{{}}; */
      static std::unique_ptr<buffer_manager[]> instances{
          new buffer_manager[number_instances]};
      return instances;
    }
    static void init_callbacks_once(void) {
      static std::once_flag flag;
      std::call_once(flag, []() {
        buffer_recycler::add_total_cleanup_callback(clean);
        buffer_recycler::add_partial_cleanup_callback(
            clean_unused_buffers_only);
          });
    }


  public:
    ~buffer_manager() {
      // All operations should have finished before this is happening
      // Should be fine when throwing as there's no real point in recovering at that stage
      // TODO mutex here is a bad idea as the HPX runtime is already shut down
      /* std::lock_guard<mutex_t> guard(mut); */ 

#ifdef CPPUDDLE_HAVE_COUNTERS
      if (number_allocation == 0 && number_recycling == 0 &&
          number_bad_alloc == 0 && number_creation == 0 &&
          unused_buffer_list.empty() && buffer_map.empty()) {
        return;
      }
#endif
      for (auto &buffer_tuple : unused_buffer_list) {
        Host_Allocator alloc;
        if (std::get<3>(buffer_tuple)) {
          std::destroy_n(std::get<0>(buffer_tuple), std::get<1>(buffer_tuple));
        }
        alloc.deallocate(std::get<0>(buffer_tuple), std::get<1>(buffer_tuple));
      }
      for (auto &map_tuple : buffer_map) {
        auto buffer_tuple = map_tuple.second;
        Host_Allocator alloc;
        if (std::get<3>(buffer_tuple)) {
          std::destroy_n(std::get<0>(buffer_tuple), std::get<1>(buffer_tuple));
        }
        alloc.deallocate(std::get<0>(buffer_tuple), std::get<1>(buffer_tuple));
      }
#ifdef CPPUDDLE_HAVE_COUNTERS
      // Print performance counters
      size_t number_cleaned = unused_buffer_list.size() + buffer_map.size();
      std::cout << "\nBuffer manager destructor for (Alloc: "
                << boost::core::demangle(typeid(Host_Allocator).name()) << ", Type: "
                << boost::core::demangle(typeid(T).name())
                << "):" << std::endl
                << "--------------------------------------------------------------------"
                << std::endl
                << "--> Number of bad_allocs that triggered garbage "
                   "collection:       "
                << number_bad_alloc << std::endl
                << "--> Number of buffers that got requested from this "
                   "manager:       "
                << number_allocation << std::endl
                << "--> Number of times an unused buffer got recycled for a "
                   "request:  "
                << number_recycling << std::endl
                << "--> Number of times a new buffer had to be created for a "
                   "request: "
                << number_creation << std::endl
                << "--> Number cleaned up buffers:                             "
                   "       "
                << number_cleaned << std::endl
                << "--> Number wrong deallocation hints:                       "
                   "       "
                << number_wrong_hints << std::endl
                << "--> Number of buffers that were marked as used upon "
                   "cleanup:      "
                << buffer_map.size() << std::endl
                << "==> Recycle rate:                                          "
                   "       "
                << static_cast<float>(number_recycling) / number_allocation *
                       100.0f
                << "%" << std::endl;
#endif
      unused_buffer_list.clear();
      buffer_map.clear();
    }

  public: // Putting deleted constructors in public gives more useful error
          // messages
    // Bunch of constructors we don't need
    buffer_manager<T, Host_Allocator>(
        buffer_manager<T, Host_Allocator> const &other) = delete;
    buffer_manager<T, Host_Allocator>(
        buffer_manager<T, Host_Allocator> &&other) = delete;
  };

public:
  // Putting deleted constructors in public gives more useful error messages
  // Bunch of constructors we don't need
  buffer_recycler(buffer_recycler const &other) = delete;
  buffer_recycler operator=(buffer_recycler const &other) = delete;
  buffer_recycler(buffer_recycler &&other) = delete;
  buffer_recycler operator=(buffer_recycler &&other) = delete;
};

template <typename T, typename Host_Allocator> struct recycle_allocator {
  using value_type = T;
  recycle_allocator() noexcept = default;
  template <typename U>
  explicit recycle_allocator(
      recycle_allocator<U, Host_Allocator> const &) noexcept {}
  T *allocate(std::size_t n) {
    T *data = buffer_recycler::get<T, Host_Allocator>(n);
    return data;
  }
  void deallocate(T *p, std::size_t n) {
    buffer_recycler::mark_unused<T, Host_Allocator>(p, n);
  }
  template <typename... Args>
  inline void construct(T *p, Args... args) noexcept {
    ::new (static_cast<void *>(p)) T(std::forward<Args>(args)...);
  }
  void destroy(T *p) { p->~T(); }
};
template <typename T, typename U, typename Host_Allocator>
constexpr bool
operator==(recycle_allocator<T, Host_Allocator> const &,
           recycle_allocator<U, Host_Allocator> const &) noexcept {
  if constexpr (std::is_same_v<T, U>)
    return true;
  else 
    return false;
}
template <typename T, typename U, typename Host_Allocator>
constexpr bool
operator!=(recycle_allocator<T, Host_Allocator> const &,
           recycle_allocator<U, Host_Allocator> const &) noexcept {
  if constexpr (std::is_same_v<T, U>)
    return false;
  else 
    return true;
}

/// Recycles not only allocations but also the contents of a buffer
template <typename T, typename Host_Allocator>
struct aggressive_recycle_allocator {
  using value_type = T;
  aggressive_recycle_allocator() noexcept = default;
  template <typename U>
  explicit aggressive_recycle_allocator(
      aggressive_recycle_allocator<U, Host_Allocator> const &) noexcept {}
  T *allocate(std::size_t n) {
    T *data = buffer_recycler::get<T, Host_Allocator>(
        n, true); // also initializes the buffer if it isn't reused
    return data;
  }
  void deallocate(T *p, std::size_t n) {
    buffer_recycler::mark_unused<T, Host_Allocator>(p, n);
  }
  template <typename... Args>
  inline void construct(T *p, Args... args) noexcept {
    // Do nothing here - we reuse the content of the last owner
  }
  void destroy(T *p) {
    // Do nothing here - Contents will be destroyed when the buffer manager is
    // destroyed, not before
  }
};
template <typename T, typename U, typename Host_Allocator>
constexpr bool
operator==(aggressive_recycle_allocator<T, Host_Allocator> const &,
           aggressive_recycle_allocator<U, Host_Allocator> const &) noexcept {
  if constexpr (std::is_same_v<T, U>)
    return true;
  else 
    return false;
}
template <typename T, typename U, typename Host_Allocator>
constexpr bool
operator!=(aggressive_recycle_allocator<T, Host_Allocator> const &,
           aggressive_recycle_allocator<U, Host_Allocator> const &) noexcept {
  if constexpr (std::is_same_v<T, U>)
    return false;
  else 
    return true;
}

} // namespace detail

template <typename T, std::enable_if_t<std::is_trivial<T>::value, int> = 0>
using recycle_std = detail::recycle_allocator<T, std::allocator<T>>;
template <typename T, std::enable_if_t<std::is_trivial<T>::value, int> = 0>
using aggressive_recycle_std =
    detail::aggressive_recycle_allocator<T, std::allocator<T>>;

/// Deletes all buffers (even ones still marked as used), delete the buffer
/// managers and the recycler itself
inline void force_cleanup() { detail::buffer_recycler::clean_all(); }
/// Deletes all buffers currently marked as unused
inline void cleanup() { detail::buffer_recycler::clean_unused_buffers(); }

} // end namespace recycler

#endif
