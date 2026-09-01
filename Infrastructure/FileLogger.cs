namespace VoiceTableAssist.Infrastructure;

/// <summary>文件日志提供程序，按日期滚动写入 logs/ 目录。</summary>
internal sealed class FileLoggerProvider : ILoggerProvider
{
    private readonly string _directory;

    public FileLoggerProvider(string directory) => _directory = directory;

    public ILogger CreateLogger(string categoryName) => new FileLogger(_directory, categoryName);

    public void Dispose() { }

    private sealed class FileLogger : ILogger
    {
        private static readonly object Gate = new();
        private readonly string _directory;
        private readonly string _category;

        public FileLogger(string directory, string category)
        {
            _directory = directory;
            _category = category;
        }

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => logLevel >= LogLevel.Information;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception, Func<TState, Exception?, string> formatter)
        {
            if (!IsEnabled(logLevel)) return;

            var message = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{logLevel}] [{_category}] {formatter(state, exception)}";
            if (exception is not null) message += Environment.NewLine + exception;

            lock (Gate)
            {
                Directory.CreateDirectory(_directory);
                File.AppendAllText(
                    Path.Combine(_directory, $"app-{DateTime.Now:yyyyMMdd}.log"),
                    message + Environment.NewLine);
            }
        }
    }
}