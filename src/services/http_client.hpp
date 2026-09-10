#ifndef LILYGO_UI_MAPS_SERVICES_HTTP_CLIENT_HPP
#define LILYGO_UI_MAPS_SERVICES_HTTP_CLIENT_HPP

#include <atomic>
#include <cstddef>
#include <mutex>
#include <set>
#include <string>
#include <sys/types.h>

struct HttpResponse {
    std::string body;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error.empty();
    }
};

class HttpClient {
public:
    HttpClient() = default;
    ~HttpClient();

    HttpClient(const HttpClient &)            = delete;
    HttpClient &operator=(const HttpClient &) = delete;

    [[nodiscard]] HttpResponse get(const std::string &url, const std::string &accept,
                                   std::size_t maximum_size);
    void cancel() noexcept;

private:
    void add_process(pid_t process) noexcept;
    void remove_process(pid_t process) noexcept;

    std::atomic_bool stopping_{false};
    std::mutex processes_mutex_;
    std::set<pid_t> processes_;
};

#endif
