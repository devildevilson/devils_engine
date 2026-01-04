#ifndef DEVILS_ENGINE_UTILS_TIME_H
#define DEVILS_ENGINE_UTILS_TIME_H

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <ctime>
#include <string>
#include <filesystem>
#include <functional>
#include <source_location>

namespace fs = std::filesystem;

namespace devils_engine {
namespace utils {
  using timestamp_t = size_t;
  using unix_timestamp_t = size_t;

  class time_log {
  public:
    std::string_view str;
    std::chrono::steady_clock::time_point tp;

    time_log(const std::string_view &str) noexcept;
    ~time_log() noexcept;
    void update_tp();

    time_log(const time_log &copy) noexcept = delete;
    time_log(time_log &&move) noexcept = delete;
    time_log & operator=(const time_log &copy) noexcept = delete;
    time_log & operator=(time_log &&move) noexcept = delete;
  };

  class trace_time_log {
  public:
    std::string_view str;
    std::chrono::steady_clock::time_point tp;
    std::source_location l;

    trace_time_log(const std::string_view& str, std::source_location loc = std::source_location::current()) noexcept;
    ~trace_time_log() noexcept;
    void update_tp();

    trace_time_log(const trace_time_log& copy) noexcept = delete;
    trace_time_log(trace_time_log&& move) noexcept = delete;
    trace_time_log& operator=(const trace_time_log& copy) noexcept = delete;
    trace_time_log& operator=(trace_time_log&& move) noexcept = delete;
  };

#ifdef DEBUGTRACE
#define DS_TIME_TRACEOBJ(msg) devils_engine::utils::trace_time_log __trace_obj(msg);
#else
#define DS_TIME_TRACEOBJ(msg)
#endif

#define FORCE_DS_TIME_TRACEOBJ(msg) devils_engine::utils::trace_time_log __trace_obj(msg);

  // возвращаем unix timestamp (наверное будет приходить по локальному времени...)
  // unix timestamp это количество секунд !!! не путать с игровым стемпом
  // нужен доступ к локалтайму
  unix_timestamp_t timestamp() noexcept;
  unix_timestamp_t file_timestamp(const fs::directory_entry &e) noexcept;

  // как именно локалтайм брать я не понимаю
  /*inline unix_timestamp_t localtime() noexcept {
    const auto p1 = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(p1);
    std::put_time();
    return std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count();
  }*/

  constexpr char const *const ISO_date_format = "%F";
  constexpr char const *const ISO_time_format = "%T";
  constexpr char const *const ISO_offset_from_utc = "%z";
  constexpr char const *const ISO_datetime_format = "%F %T";
  constexpr char const *const ISO_datetime_filename_valid_format = "%F_%H-%M-%S";
  constexpr char const *const ISO_full_datetime_format = "%F %T %z";

  size_t format_UTC(const char *format, char* buffer, const size_t max_size) noexcept;
  size_t format_localtime(const char *format, char* buffer, const size_t max_size) noexcept;
  size_t format_UTC(const unix_timestamp_t ts, const char *format, char* buffer, const size_t max_size) noexcept;
  size_t format_localtime(const unix_timestamp_t ts, const char *format, char* buffer, const size_t max_size) noexcept;

  std::string format_UTC(const char *format) noexcept;
  std::string format_localtime(const char *format) noexcept;
  std::string format_UTC(const unix_timestamp_t ts, const char *format) noexcept;
  std::string format_localtime(const unix_timestamp_t ts, const char *format) noexcept;

  template <typename F, typename... Args>
  auto perf(const std::string_view &msg, F f, Args &&...args) {
    time_log l(msg);
    return std::invoke(f, std::forward<Args>(args)...);
  }

  template <typename T>
  int64_t count_mcs(const T &tp1, const T &tp2) {
    const auto dur = tp2 - tp1;
    return std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
  }

  // при этом в игре у нас потенциально могут быть заданы иные правила для игрового дня
  struct date {
    int32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t week_day;

    date() noexcept;
    date(const int32_t year, const uint32_t month, const uint32_t day, const uint32_t hour, const uint32_t minute, const uint32_t second, const uint32_t week_day)  noexcept;
    date(const int32_t year, const uint32_t month, const uint32_t day, const uint32_t week_day = 0) noexcept;
    date(const int32_t year, const uint32_t week_day = 0)  noexcept;
  };

  // наверное наиболее точным форматом будет указать количество секунд в году
  // и от этого отталкиваться
  class date_system {
  public:
    struct month {
      std::string name;
      uint32_t days;
      int32_t leap_day; // years
    };

    date_system() noexcept;
    void init(std::vector<month> months, std::vector<std::string> week, const uint32_t hours_in_day = 24, const uint32_t minutes_in_hour = 60, const uint32_t seconds_in_minute = 60) noexcept;
    void set_start_date(const date &d) noexcept;

    bool is_valid(const date &d) const;
    timestamp_t cast(const date &d) const;
    date cast(const timestamp_t &t) const;
    timestamp_t add(const timestamp_t cur, int32_t years, int32_t months, int32_t days) const;

    date initial_date() const;
    std::string_view month_str(const uint32_t index) const;
    std::string_view month_str(const date &d) const;
    std::string_view week_day_str(const uint32_t index) const;
    std::string_view week_day_str(const date &d) const;
    bool is_leap_year(const int32_t year) const;
    bool is_leap_year(const date &d) const;

    size_t date_count_days(const date &d) const;
    size_t week_number(const date &d) const;
    size_t days_to_next_year(const date &d) const;

    size_t year_days(const int32_t year) const;
    size_t month_days(const int32_t year, const uint32_t month) const;
    size_t day_seconds() const;
    size_t current_day_seconds(const date &d) const;
  private:
    date start_date;
    std::vector<month> months;
    std::vector<std::string> week;
    uint32_t hours_in_day;
    uint32_t minutes_in_hour;
    uint32_t seconds_in_minute;
  };

  class game_date {
  public:
    static const date_system & get();
    void init(std::vector<date_system::month> months, std::vector<std::string> week, const uint32_t hours_in_day = 24, const uint32_t minutes_in_hour = 60, const uint32_t seconds_in_minute = 60);
    void set_start_date(const date &d);
  private:
    static date_system s;
  };

  // так мне еще нужна структура которая посчитает относительное время и этим временем будут пользоваться все остальные системы в приложении
  // для этого времени есть пауза в меню (для одиночной игры)
  // какое разрешение у timestamp_t? удобно брать микросекунды
  struct app_clock {
    timestamp_t start_time;
    timestamp_t elapsed_time;

    static size_t resolution(); // mcs

    app_clock(const timestamp_t start_time) noexcept;
    void update(const size_t delta);

    timestamp_t timestamp() const;
    int64_t diff(const timestamp_t timestamp) const;
    double seconds() const;
  };

  // наверное этот тип будет полезнее чем app_clock
  struct app_timer {
    timestamp_t start_time;
    size_t elapsed_time;
    timestamp_t end_time;

    static size_t resolution(); // mcs
    static double seconds(const int64_t time);

    app_timer(const timestamp_t start_time, const timestamp_t end_time = 0) noexcept;
    void update(const size_t delta) noexcept;

    std::tuple<timestamp_t, timestamp_t, timestamp_t> status() const noexcept;
    bool elapsed() const noexcept;
    timestamp_t timestamp() const noexcept;
    int64_t diff(const timestamp_t timestamp) const noexcept;
  };
}
}

#endif