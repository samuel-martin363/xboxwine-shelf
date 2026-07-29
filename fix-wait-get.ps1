param(
    [string]$ProjectRoot = (Get-Location).Path
)

$ErrorActionPreference = "Stop"

$entryPath = Join-Path $ProjectRoot "patch\uwp\shelf_entry.cpp"

if (-not (Test-Path $entryPath)) {
    throw "Missing $entryPath"
}

$text = [System.IO.File]::ReadAllText($entryPath)

if (-not $text.Contains("#include <fstream>")) {
    $text = $text.Replace(
        "#include <exception>",
        "#include <exception>`r`n#include <fstream>"
    )
}

$oldFunction = @'
void WriteStartupLog(const std::string& line, bool reset = false) noexcept {
    try {
        using namespace winrt;
        using namespace Windows::Storage;

        const StorageFolder local = ApplicationData::Current().LocalFolder();
        const StorageFile file = local.CreateFileAsync(
            L"xboxwine-startup.log",
            CreationCollisionOption::OpenIfExists
        ).get();

        const std::string text = line + "\r\n";
        if (reset) {
            FileIO::WriteTextAsync(file, to_hstring(text)).get();
        } else {
            FileIO::AppendTextAsync(file, to_hstring(text)).get();
        }
    } catch (...) {
        // Logging must never become another startup failure.
    }
}
'@

$newFunction = @'
void WriteStartupLog(const std::string& line, bool reset = false) noexcept {
    try {
        using namespace winrt::Windows::Storage;

        // Avoid blocking C++/WinRT async .get() calls during UWP startup.
        // The app is allowed to write synchronously inside its own LocalFolder.
        const StorageFolder local = ApplicationData::Current().LocalFolder();

        std::string path = winrt::to_string(local.Path());
        if (!path.empty() && path.back() != '\\') {
            path.push_back('\\');
        }
        path += "xboxwine-startup.log";

        const std::ios::openmode mode =
            std::ios::binary |
            std::ios::out |
            (reset ? std::ios::trunc : std::ios::app);

        std::ofstream file(path, mode);
        if (file) {
            file.write(line.data(), static_cast<std::streamsize>(line.size()));
            file.write("\r\n", 2);
            file.flush();
        }
    } catch (...) {
        // Logging must never become another startup failure.
    }
}
'@

if ($text.Contains($oldFunction)) {
    $text = $text.Replace($oldFunction, $newFunction)
}
elseif (-not $text.Contains("Avoid blocking C++/WinRT async .get() calls")) {
    throw "Could not find the expected WriteStartupLog function."
}

[System.IO.File]::WriteAllText(
    $entryPath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host "Removed the C++/WinRT wait_get build failure." -ForegroundColor Green
Write-Host "Changed: patch\uwp\shelf_entry.cpp"
