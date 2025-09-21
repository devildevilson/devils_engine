#include "time-utils.hpp"
#include "core.h"

namespace devils_engine {
namespace utils {
  time_log::time_log(const std::string_view &str) noexcept : str(str), tp(std::chrono::steady_clock::now()) {}
  time_log::~time_log() noexcept {
    const auto dur = std::chrono::steady_clock::now() - tp;
    const size_t mcs = std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
    utils::info("{} took {} mcs ({:.3} seconds)", str, mcs, double(mcs)/1000000.0);
  }

  void time_log::update_tp() { tp = std::chrono::steady_clock::now(); }

  unix_timestamp_t timestamp() noexcept {
    const auto p1 = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count();
  }

  unix_timestamp_t file_timestamp(const fs::directory_entry &e) noexcept {
    if (!e.exists()) return 0;
    const auto tp = e.last_write_time();
    const auto tp2 = std::chrono::clock_cast<std::chrono::system_clock>(tp); 
    return std::chrono::duration_cast<std::chrono::seconds>(tp2.time_since_epoch()).count();
  }

  size_t format_UTC(const char *format, char* buffer, const size_t max_size) noexcept {
    const auto p1 = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(p1);
    return std::strftime(buffer, max_size, format, std::gmtime(&t));
  }

  size_t format_localtime(const char *format, char* buffer, const size_t max_size) noexcept {
    const auto p1 = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(p1);
    return std::strftime(buffer, max_size, format, std::localtime(&t));
  }

  // надеюсь что это портабельно, хоспаде
  size_t format_UTC(const unix_timestamp_t ts, const char *format, char* buffer, const size_t max_size) noexcept {
    const auto t = std::time_t(ts);
    return std::strftime(buffer, max_size, format, std::gmtime(&t));
  }

  size_t format_localtime(const unix_timestamp_t ts, const char *format, char* buffer, const size_t max_size) noexcept {
    const auto t = std::time_t(ts);
    return std::strftime(buffer, max_size, format, std::localtime(&t));
  }

  std::string format_UTC(const char *format) noexcept {
    std::string cont(1000, '\0'); // блен такого рода контейнеры - это вечно какая то небыстрая история увы
    const size_t count = format_UTC(format, cont.data(), cont.size());
    cont.resize(count);
    return cont;
  }

  std::string format_localtime(const char *format) noexcept {
    std::string cont(1000, '\0');
    const size_t count = format_localtime(format, cont.data(), cont.size());
    cont.resize(count);
    return cont;
  }

  std::string format_UTC(const unix_timestamp_t ts, const char *format) noexcept {
    std::string cont(1000, '\0');
    const size_t count = format_UTC(ts, format, cont.data(), cont.size());
    cont.resize(count);
    return cont;
  }

  std::string format_localtime(const unix_timestamp_t ts, const char *format) noexcept {
    std::string cont(1000, '\0');
    const size_t count = format_localtime(ts, format, cont.data(), cont.size());
    cont.resize(count);
    return cont;
  }

  date::date() noexcept : year(0), month(0), day(0), hour(0), minute(0), second(0), week_day(0) {}
  date::date(const int32_t year, const uint32_t month, const uint32_t day, const uint32_t hour, const uint32_t minute, const uint32_t second, const uint32_t week_day) noexcept 
    : year(year), month(month), day(day), hour(hour), minute(minute), second(second), week_day(week_day)
  {}

  date::date(const int32_t year, const uint32_t month, const uint32_t day, const uint32_t week_day) noexcept 
    : date(year, month, day, 0, 0, 0, week_day)
  {}

  date::date(const int32_t year, const uint32_t week_day) noexcept 
    : date(year, 0, 0, week_day)
  {}

  static const std::vector<date_system::month> default_months({
    date_system::month{ "january", 31, 0 },
    date_system::month{ "february", 28, 4 },
    date_system::month{ "march", 31, 0 },
    date_system::month{ "april", 30, 0 },
    date_system::month{ "may", 31, 0 },
    date_system::month{ "june", 30, 0 },
    date_system::month{ "july", 31, 0 },
    date_system::month{ "august", 31, 0 },
    date_system::month{ "september", 30, 0 },
    date_system::month{ "october", 31, 0 },
    date_system::month{ "november", 30, 0 },
    date_system::month{ "december", 31, 0 }
  });

  static const std::vector<std::string> default_week({
    "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"
  });

  date_system::date_system() noexcept : months(default_months), week(default_week), hours_in_day(24), minutes_in_hour(60), seconds_in_minute(60) {}
  void date_system::init(std::vector<month> months, std::vector<std::string> week, const uint32_t hours_in_day, const uint32_t minutes_in_hour, const uint32_t seconds_in_minute) noexcept {
    this->months = std::move(months);
    this->week = std::move(week);
    this->hours_in_day = hours_in_day;
    this->minutes_in_hour = minutes_in_hour;
    this->seconds_in_minute = seconds_in_minute;
  }

  void date_system::set_start_date(const date &d) noexcept {
    this->start_date = d;
  }

  static int32_t sign(const double a) {
    return a >= 0 ? 1 : -1;
  }

  // недостаточная проверка валидности !!!!!!!!! но будет работать если задавать только стартовый год
  bool date_system::is_valid(const date &d) const {
    if (d.year < start_date.year) return false;
    if (d.month >= months.size()) return false;
    const auto &m = months[d.month];
    const uint32_t month_days = m.days + sign(m.leap_day) * int32_t(m.leap_day != 0 && d.year % std::abs(m.leap_day) == 0);
    if (d.day >= month_days) return false;
    if (d.hour >= hours_in_day) return false;
    if (d.minute >= minutes_in_hour) return false;
    if (d.second >= seconds_in_minute) return false;
    // если стартовая дата будет НЕ первое января то все сломается
    return true;
  }

  // каст надо переписать полностью, я так понимаю сначала нужно 
  // 1) добавить дни до 1 января следующего года
  // 2) пройтись по годам до дейт год -1
  // 3) пройтись по месяцам до месяца -1
  // 4) добавить остаток дней
  timestamp_t date_system::cast(const date &d) const {
    if (!is_valid(d)) return SIZE_MAX;

    auto cur = start_date;
    size_t seconds = 0;
    size_t days = 0;
    // если СТАРТОВАЯ ДАТА не стандартная
    if (cur.month == 0 && cur.day == 0 && cur.hour == 0 && cur.minute == 0 && cur.second == 0) {
      // 1 января
    } else {
      seconds += seconds_in_minute - cur.second;
      cur.second = 0;
      cur.minute += 1;

      seconds += (minutes_in_hour - cur.minute) * seconds_in_minute;
      cur.minute = 0;
      cur.hour += 1;

      seconds += (hours_in_day - cur.hour) * minutes_in_hour * seconds_in_minute;
      cur.hour = 0;
      cur.day += 1;
      days += 1;

      if (cur.year < d.year) {
        days += days_to_next_year(cur);

        cur.day = 0;
        cur.month = 0;
        cur.year += 1;
      }
    }

    // если год не отличается то что? чист добираем дни и секунды

    while (cur.year < d.year) {
      const size_t days_in_year = year_days(cur.year);
      days += days_in_year;
      cur.year += 1;
    }

    while (cur.month < d.month) {
      const uint32_t cur_month_days = month_days(cur.year, cur.month);
      days += cur_month_days;
      cur.month += 1;
    }

    days += d.day;

    seconds += current_day_seconds(d);
    
    const timestamp_t t = (days * day_seconds() + seconds) * app_clock::resolution();
    //utils::info("date_to_t s {} add_days {}", (days * day_seconds() + seconds), days);
    return t;
  }

  date date_system::cast(const timestamp_t &t) const {
    auto d = start_date;
    const size_t seconds_in_day = day_seconds();
    // начнем с того что посчитаем всего секунд
    size_t stamp_seconds = double(t) / double(app_clock::resolution());
    size_t add_days = double(stamp_seconds) / double(seconds_in_day);
    //utils::info("t_to_date s {} add_days {}", stamp_seconds, add_days);

    // тут тоже нужно привести стартовую дату к 1 января и начала прибавить года, месяцы, дни и проч
    // если СТАРТОВАЯ ДАТА не стандартная
    if (d.month == 0 && d.day == 0 && d.hour == 0 && d.minute == 0 && d.second == 0) {
      // 1 января
    } else {
      stamp_seconds -= seconds_in_minute - d.second;
      d.second = 0;
      d.minute += 1;

      stamp_seconds -= (minutes_in_hour - d.minute) * seconds_in_minute;
      d.minute = 0;
      d.hour += 1;

      stamp_seconds -= (hours_in_day - d.hour) * minutes_in_hour * seconds_in_minute;
      d.hour = 0;
      d.day += 1;
      add_days -= 1;

      const size_t to_next = days_to_next_year(d);
      if (add_days >= to_next) {
        add_days -= days_to_next_year(d);

        d.day = 0;
        d.month = 0;
        d.year += 1;
      }
    }

    // теперь нужно прибавить секунды к стартовой дате
    const size_t seconds_part = stamp_seconds % seconds_in_day;
    const size_t add_seconds = seconds_part % seconds_in_minute;
    const size_t minutes_part = seconds_part / minutes_in_hour;
    size_t add_minutes = minutes_part % minutes_in_hour;
    size_t hours_part = minutes_part / hours_in_day;

    d.second += add_seconds;
    if (d.second >= seconds_in_minute) {
      d.second -= seconds_in_minute;
      d.minute += 1;
    }

    d.minute += add_minutes;
    if (d.minute >= minutes_in_hour) {
      d.minute -= minutes_in_hour;
      d.hour += 1;
    }

    d.hour += hours_part;
    if (d.hour >= hours_in_day) {
      d.hour -= hours_in_day;
      add_days += 1;
    }

    size_t cur_year_days = year_days(d.year);
    while (add_days >= cur_year_days) {
      add_days -= cur_year_days;
      d.year += 1;
      d.week_day = (d.week_day + cur_year_days) % week.size();
      cur_year_days = year_days(d.year);
    }

    size_t cur_month_days = month_days(d.year, d.month);
    while (add_days >= cur_month_days) {
      add_days -= cur_month_days;
      d.month += 1;
      d.week_day = (d.week_day + cur_month_days) % week.size();
      cur_month_days = month_days(d.year, d.month);
    }

    d.day = add_days;
    d.week_day = (d.week_day + add_days) % week.size();

    return d;
  }

  // чутка долго получается, как быстрее? сразу пачками прибавлять/вычитать
  // там увы не очень очевидно как сделать для двух знаков
  // прям супер тяжелая функция
  timestamp_t date_system::add(const timestamp_t cur, int32_t years, int32_t months, int32_t days) const {
    auto cur_date = cast(cur);

    cur_date.year += years;

    while (months != 0) {
      const auto s = sign(months);
      if (months >= this->months.size()) {
        months = s * (std::abs(months) - this->months.size());
        cur_date.year += s;
      } else {
        if (s > 0) {
          cur_date.year += uint32_t(cur_date.month + months > this->months.size());
          cur_date.month = (cur_date.month + months) % this->months.size();
        } else {
          cur_date.year -= int32_t(int32_t(cur_date.month) - std::abs(months) < 0);
          cur_date.month = (int32_t(cur_date.month) - std::abs(months)) % this->months.size();
        }
        months = 0;
      }
    }

    if (days != 0 && cur_date.day != 0) {
      const auto s = sign(days);
      const int32_t abs_days = std::abs(days);
      const uint32_t cur_month_days = month_days(cur_date.year, cur_date.month);
      const int32_t days_part = s > 0 ? cur_month_days - cur_date.day : cur_date.day;
      days = s * (abs_days - std::min(days_part, abs_days));
      cur_date.day += s * std::min(days_part, abs_days);
    }

    while (days != 0) {
      const auto s = sign(days);
      const int32_t abs_days = std::abs(days);
      
      // берем предыдущий месяц М
      const int32_t m_year = s > 0 ? cur_date.year : (cur_date.month != 0 ? cur_date.year : cur_date.year - 1);
      const int32_t m_month = s > 0 ? cur_date.month : (cur_date.month != 0 ? cur_date.month - 1 : this->months.size()-1);

      cur_date.year = m_year;
      cur_date.month = m_month;

      const uint32_t m_month_days = month_days(m_year, m_month);
      if (abs_days >= m_month_days) days = s * (abs_days - m_month_days);
      else {
        days = 0;
        cur_date.day = s > 0 ? cur_date.day + abs_days : m_month_days - abs_days;
      }
    }

    return cast(cur_date);
  }

  date date_system::initial_date() const { return start_date; }
  std::string_view date_system::month_str(const uint32_t index) const { return index < months.size() ? std::string_view(months[index].name) : std::string_view(); }
  std::string_view date_system::week_day_str(const uint32_t index) const { return index < week.size() ? std::string_view(week[index]) : std::string_view(); }
  std::string_view date_system::month_str(const date &d) const { return month_str(d.month); }
  std::string_view date_system::week_day_str(const date &d) const { return week_day_str(d.week_day); }
  bool date_system::is_leap_year(const int32_t year) const {
    for (const auto &m : months) {
      const uint32_t leap_day = sign(m.leap_day) * int32_t(m.leap_day != 0 && (year % std::abs(m.leap_day) == 0));
      if (leap_day != 0) return true;
    }

    return false;
  }

  bool date_system::is_leap_year(const date &d) const { return is_leap_year(d.year); }

  size_t date_system::date_count_days(const date &d) const {
    size_t count = 0;
    for (uint32_t i = 0; i < d.month+1; ++i) {
      count += month_days(d.year, i);
    }

    count += d.day+1;
    return count;
  }
  
  size_t date_system::week_number(const date &d) const {
    return date_count_days(d) / week.size();
  }

  size_t date_system::days_to_next_year(const date &d) const {
    size_t days = 0;
    auto cur = d;
    for (uint32_t i = cur.month; i < months.size(); ++i) {
      const uint32_t cur_month_days = month_days(cur.year, i);

      days += cur_month_days - cur.day;
      cur.day = 0;
    }

    return days;
  }

  size_t date_system::year_days(const int32_t year) const {
    size_t days = 0;
    for (uint32_t i = 0; i < months.size(); ++i) {
      days += month_days(year, i);
    }

    return days;
  }

  size_t date_system::month_days(const int32_t year, const uint32_t month) const {
    if (month >= months.size()) return SIZE_MAX;
    const auto &m = this->months[month];
    const uint32_t month_days = m.days + sign(m.leap_day) * int32_t(m.leap_day != 0 && (year % std::abs(m.leap_day) == 0));
    return month_days;
  }

  size_t date_system::day_seconds() const {
    return size_t(hours_in_day) * size_t(minutes_in_hour) * size_t(seconds_in_minute);
  }

  size_t date_system::current_day_seconds(const date &d) const {
    return size_t(d.hour) * size_t(minutes_in_hour) * size_t(seconds_in_minute) + size_t(d.minute) * size_t(seconds_in_minute) + size_t(d.second);
  }

  const date_system &game_date::get() { return s; }
  //const date_system &game_date::operator()() { return s; }
  void game_date::init(std::vector<date_system::month> months, std::vector<std::string> week, const uint32_t hours_in_day, const uint32_t minutes_in_hour, const uint32_t seconds_in_minute) {
    s.init(std::move(months), std::move(week), hours_in_day, minutes_in_hour, seconds_in_minute);
  }

  void game_date::set_start_date(const date &d) { s.set_start_date(d); }

  date_system game_date::s;

  size_t app_clock::resolution() { return 1000000; }

  app_clock::app_clock(const timestamp_t start_time) noexcept : start_time(start_time), elapsed_time(0) {}
  void app_clock::update(const size_t delta) { elapsed_time += delta; }

  timestamp_t app_clock::timestamp() const { return start_time + elapsed_time; }
  int64_t app_clock::diff(const timestamp_t timestamp) const {
    return this->timestamp() < timestamp ? timestamp - this->timestamp() : -(this->timestamp() - timestamp);
  }

  double app_clock::seconds() const { return double(this->timestamp()) / double(resolution()); }

  size_t app_timer::resolution() { return 1000000; }
  double app_timer::seconds(const int64_t time) { return double(time) / double(resolution()); }

  app_timer::app_timer(const timestamp_t start_time, const timestamp_t end_time) noexcept : start_time(start_time), elapsed_time(0), end_time(end_time) {}
  void app_timer::update(const size_t delta) noexcept { elapsed_time += delta; }

  // а зачем тут elapsed time? если сильно нужно то у меня для этого start_time - timestamp, скорее тут можно вернуть время окончания
  // правильно мы в функцию будем пихать дельту, текущий таймстемп, старт?, конец?
  std::tuple<timestamp_t, timestamp_t, timestamp_t> app_timer::status() const noexcept { return std::make_tuple(timestamp(), start_time, end_time); }
  bool app_timer::elapsed() const noexcept { return elapsed_time != 0 && timestamp() >= elapsed_time; }
  timestamp_t app_timer::timestamp() const noexcept { return start_time + elapsed_time; }
  int64_t app_timer::diff(const timestamp_t timestamp) const noexcept { return int64_t(timestamp) - int64_t(this->timestamp()); }
}
}