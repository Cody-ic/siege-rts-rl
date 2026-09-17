using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;

[assembly: AssemblyTitle("Sanctum Portable")]
[assembly: AssemblyDescription("Single-file wrapper for the unchanged Sanctum v1.0.1 release")]
[assembly: AssemblyVersion("1.0.1.0")]
[assembly: AssemblyFileVersion("1.0.1.0")]

internal static class PortableLauncher
{
    private const string PayloadHash = "2de9442252d3f5f741f2f002f4e376ae926c07451a4c8186782716356e07510a";
    private const string PackageRoot = "Sanctum-1.0.1-Windows-x64";

    // Windows command-line quoting, including empty arguments and trailing slashes.
    internal static string Quote(string value)
    {
        var result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in value)
        {
            if (c == '\\') { ++slashes; continue; }
            if (c == '"')
            {
                result.Append('\\', slashes * 2 + 1);
                result.Append('"');
            }
            else { result.Append('\\', slashes); result.Append(c); }
            slashes = 0;
        }
        result.Append('\\', slashes * 2);
        return result.Append('"').ToString();
    }

    private static void Extract(string destination)
    {
        using (Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
        {
            if (payload == null) throw new InvalidDataException("Embedded game package is missing.");
            using (var sha = SHA256.Create())
            {
                string hash = BitConverter.ToString(sha.ComputeHash(payload)).Replace("-", "").ToLowerInvariant();
                if (hash != PayloadHash) throw new InvalidDataException("Embedded game package failed SHA256 validation.");
            }
            payload.Position = 0;
            using (var archive = new ZipArchive(payload, ZipArchiveMode.Read))
            {
                string prefix = Path.GetFullPath(destination) + Path.DirectorySeparatorChar;
                foreach (var entry in archive.Entries)
                {
                    // Reject absolute paths, alternate data streams and traversal.
                    string relative = entry.FullName.Replace('/', Path.DirectorySeparatorChar);
                    if (Path.IsPathRooted(relative) || relative.IndexOf(':') >= 0)
                        throw new InvalidDataException("Invalid archive path.");
                    string target = Path.GetFullPath(Path.Combine(destination, relative));
                    if (!target.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException("Archive path escapes the extraction directory.");
                    if (String.IsNullOrEmpty(entry.Name)) { Directory.CreateDirectory(target); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    using (Stream input = entry.Open())
                    using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                        input.CopyTo(output);
                }
            }
        }
    }

    [STAThread]
    private static int Main(string[] args)
    {
        string work = null;
        string localData = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        if (String.IsNullOrEmpty(localData))
            localData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string logDirectory = Path.Combine(localData, "SiegeRTS");
        string log = Path.Combine(logDirectory, "portable.log");
        try
        {
            if (!Environment.Is64BitOperatingSystem)
                throw new PlatformNotSupportedException("Windows 10/11 64-bit is required.");
            Directory.CreateDirectory(logDirectory);
            // A private per-launch directory avoids stale caches and overlapping cleanup.
            work = Path.Combine(logDirectory, "portable", "run-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(work);
            File.AppendAllText(log, DateTime.UtcNow.ToString("o") + " Extracting v1.0.1 to " + work + Environment.NewLine);
            Extract(work);
            string root = Path.Combine(work, PackageRoot);
            string launcher = Path.Combine(root, "圣城.exe");
            if (!File.Exists(launcher)) throw new FileNotFoundException("Original game launcher was not found.");
            var command = new StringBuilder();
            foreach (string arg in args) { if (command.Length > 0) command.Append(' '); command.Append(Quote(arg)); }
            var start = new ProcessStartInfo(launcher, command.ToString());
            start.WorkingDirectory = root;
            start.UseShellExecute = false;
            using (var child = Process.Start(start))
            {
                if (child == null) throw new InvalidOperationException("Could not start the game.");
                child.WaitForExit();
                File.AppendAllText(log, "Game exit code: " + child.ExitCode + Environment.NewLine);
                return child.ExitCode;
            }
        }
        catch (Exception error)
        {
            try { Directory.CreateDirectory(logDirectory); File.AppendAllText(log, error.ToString() + Environment.NewLine); }
            catch { }
            MessageBox.Show("单文件版启动失败：\n" + error.Message + "\n\n诊断日志：" + log,
                "圣城 v1.0.1-Portable", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
        finally
        {
            // Never delete saves or any directory belonging to a different launch.
            if (work != null)
            {
                try { Directory.Delete(work, true); }
                catch (Exception error)
                {
                    try { File.AppendAllText(log, "Temporary files retained: " + work + "\n" + error.Message + Environment.NewLine); }
                    catch { }
                }
            }
        }
    }
}
