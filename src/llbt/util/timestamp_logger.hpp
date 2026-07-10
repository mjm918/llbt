
#ifndef LLBT_UTIL_TIMESTAMP_LOGGER_HPP
#define LLBT_UTIL_TIMESTAMP_LOGGER_HPP

#include <llbt/util/logger.hpp>
#include <llbt/util/timestamp_formatter.hpp>


namespace llbt {
namespace util {

class TimestampStderrLogger : public Logger {
public:
    using Precision = TimestampFormatter::Precision;
    using Config = TimestampFormatter::Config;

    explicit TimestampStderrLogger(Config = {}, Level = LogCategory::barq.get_default_level_threshold());

protected:
    void do_log(const LogCategory& category, Logger::Level, const std::string& message) final;

private:
    TimestampFormatter m_formatter;
};


} // namespace util
} // namespace llbt

#endif // LLBT_UTIL_TIMESTAMP_LOGGER_HPP
