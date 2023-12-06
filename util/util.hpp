#ifndef CM_UTIL_UTIL_HPP
#define CM_UTIL_UTIL_HPP

#define get_thread_id std::hash<std::thread::id>{}(std::this_thread::get_id())


#endif