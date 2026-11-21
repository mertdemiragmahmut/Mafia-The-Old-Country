using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace MafiaTOCDualSenseInstaller
{
    internal static class Program
    {
        private const string ResourceName = "MafiaTOCDualSensePayload.zip";
        private const string ExeRelPath = @"MafiaTheOldCountry\Binaries\Win64\MafiaTheOldCountry.exe";
        private const string BinRelDir = @"MafiaTheOldCountry\Binaries\Win64";
        private const string ManifestRelPath = @"MafiaTheOldCountry\Binaries\Win64\ue4ss\MafiaDualSenseMod.manifest.txt";

        private static readonly string[] FallbackModFiles = new[]
        {
            @"MafiaTheOldCountry\Binaries\Win64\dwmapi.dll",
            @"MafiaTheOldCountry\Binaries\Win64\xinput1_4.dll",
            @"MafiaTheOldCountry\Binaries\Win64\hid.dll",
            @"MafiaTheOldCountry\Binaries\Win64\DualSenseBridge.log",
            @"MafiaTheOldCountry\Binaries\Win64\DualSenseHidProxy.log",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\DualSenseBridge.log",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\DualSenseHidProxy.log",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS.log",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS.dll",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\LICENSE",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS-settings.ini",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\MemberVariableLayout.ini",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\MafiaDualSenseMod.manifest.txt",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures\GUObjectArray.lua",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures\GUObjectHashTables.lua",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures\GNatives.lua",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures\FName_Constructor.lua",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures\FName_ToString.lua",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures\GameEngineTick.lua",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures\ConsoleManager.lua",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\mods.txt",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\shared\UEHelpers\UEHelpers.lua",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\MafiaDualSenseMod\enabled.txt",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\MafiaDualSenseMod\Scripts\main.lua"
        };

        private static readonly string[] CleanupSubDirs = new[]
        {
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\MafiaDualSenseMod\Scripts",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\MafiaDualSenseMod",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\shared\UEHelpers",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\shared",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures",
            @"MafiaTheOldCountry\Binaries\Win64\ue4ss"
        };

        // Win32 HID detection via SetupAPI
        private const uint DIGCF_PRESENT = 0x00000002;
        private const uint DIGCF_DEVICEINTERFACE = 0x00000010;
        private static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential)]
        private struct SP_DEVICE_INTERFACE_DATA
        {
            public int cbSize;
            public Guid InterfaceClassGuid;
            public uint Flags;
            public UIntPtr Reserved;
        }

        [DllImport("hid.dll", SetLastError = true)]
        private static extern void HidD_GetHidGuid(out Guid hidGuid);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SetupDiGetClassDevsW(
            ref Guid ClassGuid,
            string Enumerator,
            IntPtr hwndParent,
            uint Flags);

        [DllImport("setupapi.dll", SetLastError = true)]
        private static extern bool SetupDiEnumDeviceInterfaces(
            IntPtr DeviceInfoSet,
            IntPtr DeviceInfoData,
            ref Guid InterfaceClassGuid,
            uint MemberIndex,
            ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool SetupDiGetDeviceInterfaceDetailW(
            IntPtr DeviceInfoSet,
            ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
            IntPtr DeviceInterfaceDetailData,
            uint DeviceInterfaceDetailDataSize,
            out uint RequiredSize,
            IntPtr DeviceInfoData);

        [DllImport("setupapi.dll", SetLastError = true)]
        private static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

        private static int Main(string[] args)
        {
            Console.OutputEncoding = Encoding.UTF8;
            Console.InputEncoding = Encoding.UTF8;
            Console.Title = "Mafia: The Old Country - DualSense Mod Kurulum & Kaldırma Aracı";

            string mode = null;
            string explicitGameDir = null;

            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i].Trim();
                if (a.Equals("--install", StringComparison.OrdinalIgnoreCase) || a.Equals("/install", StringComparison.OrdinalIgnoreCase))
                    mode = "install";
                else if (a.Equals("--uninstall", StringComparison.OrdinalIgnoreCase) || a.Equals("/uninstall", StringComparison.OrdinalIgnoreCase))
                    mode = "uninstall";
                else if (a.Equals("--status", StringComparison.OrdinalIgnoreCase) || a.Equals("/status", StringComparison.OrdinalIgnoreCase))
                    mode = "status";
                else if (a.Equals("--help", StringComparison.OrdinalIgnoreCase) || a.Equals("-h", StringComparison.OrdinalIgnoreCase) || a.Equals("/?", StringComparison.OrdinalIgnoreCase))
                    mode = "help";
                else if (a.Equals("--game-dir", StringComparison.OrdinalIgnoreCase) && i + 1 < args.Length)
                    explicitGameDir = args[++i];
            }

            if (mode == "help")
            {
                PrintHelp();
                return 0;
            }

            string gameRoot = ResolveGameRoot(explicitGameDir);

            if (mode != null)
            {
                if (string.IsNullOrEmpty(gameRoot))
                {
                    WriteColor("HATA: 'Mafia The Old Country' oyun klasörü bulunamadı!", ConsoleColor.Red);
                    Console.WriteLine("Lütfen bu aracı oyunun kurulu olduğu klasörde çalıştırın veya --game-dir \"<yol>\" parametresini kullanın.");
                    return 2;
                }

                switch (mode)
                {
                    case "install": return RunInstall(gameRoot);
                    case "uninstall": return RunUninstall(gameRoot);
                    case "status": return RunStatus(gameRoot);
                }
                return 0;
            }

            // Interactive Turkish console menu
            return RunInteractiveMenu(gameRoot);
        }

        private static void PrintHeader()
        {
            Console.WriteLine("==================================================================");
            WriteColor("  Mafia: The Old Country - Native DualSense Mod Kur / Kaldır", ConsoleColor.Cyan);
            Console.WriteLine("  (PlayStation Sembolleri + Adaptive Trigger + Haptic + Lightbar)");
            Console.WriteLine("==================================================================");
        }

        private static void PrintHelp()
        {
            PrintHeader();
            Console.WriteLine("Kullanım:");
            Console.WriteLine("  Argümansız çalıştırma       -> Etkileşimli Türkçe menü");
            Console.WriteLine("  --install                   -> Modu oyuna kurar / günceller");
            Console.WriteLine("  --uninstall                 -> Mod dosyalarını tamamen temizler");
            Console.WriteLine("  --status                    -> Kurulum ve DualSense durumunu gösterir");
            Console.WriteLine("  --game-dir \"<oyun yolu>\"    -> Oyun kök klasörünü elle belirtir");
        }

        private static int RunInteractiveMenu(string initialRoot)
        {
            string gameRoot = initialRoot;
            while (true)
            {
                Console.WriteLine();
                PrintHeader();
                if (!string.IsNullOrEmpty(gameRoot))
                {
                    WriteColor("Oyun Klasörü : " + gameRoot, ConsoleColor.Green);
                }
                else
                {
                    WriteColor("Oyun Klasörü : BULUNAMADI (5'e basarak klasör yolu girebilirsiniz)", ConsoleColor.Yellow);
                }

                string dsDetail;
                bool dsFound = IsWiredDualSenseConnected(out dsDetail);
                WriteColor("DualSense USB: " + (dsFound ? "BAĞLI (" + dsDetail + ")" : "Algılanamadı (USB kablo ile bağlayın)"),
                    dsFound ? ConsoleColor.Green : ConsoleColor.Yellow);

                Console.WriteLine();
                Console.WriteLine("  [1] Modu Kur / Güncelle");
                Console.WriteLine("  [2] Modu Tamamen Kaldır");
                Console.WriteLine("  [3] Kurulum ve DualSense Durumunu Kontrol Et");
                Console.WriteLine("  [4] Çıkış");
                Console.WriteLine("  [5] Oyun Klasörünü Elle Seç");
                Console.Write("\nSeçiminiz (1-5): ");

                string choice = Console.ReadLine();
                if (choice == null) return 0;
                choice = choice.Trim();

                if (choice == "4") return 0;
                if (choice == "5")
                {
                    Console.Write("Oyun kök klasörü yolunu yapıştırın (örn: C:\\...\\Mafia The Old Country): ");
                    string entered = (Console.ReadLine() ?? "").Trim().Trim('"');
                    string resolved = ValidateGameDirCandidate(entered);
                    if (!string.IsNullOrEmpty(resolved))
                    {
                        gameRoot = resolved;
                        WriteColor("Oyun klasörü doğrulandı!", ConsoleColor.Green);
                    }
                    else
                    {
                        WriteColor("Bu klasörde 'MafiaTheOldCountry.exe' bulunamadı.", ConsoleColor.Red);
                    }
                    continue;
                }

                if (string.IsNullOrEmpty(gameRoot))
                {
                    WriteColor("Önce geçerli oyun klasörünü seçin ([5]).", ConsoleColor.Red);
                    continue;
                }

                if (choice == "1") RunInstall(gameRoot);
                else if (choice == "2") RunUninstall(gameRoot);
                else if (choice == "3") RunStatus(gameRoot);
                else WriteColor("Geçersiz seçim.", ConsoleColor.Yellow);
            }
        }

        private static int RunInstall(string gameRoot)
        {
            Console.WriteLine();
            PrintHeader();
            Console.WriteLine("Kurulum başlatılıyor: " + gameRoot);

            if (IsGameRunning())
            {
                WriteColor("HATA: Oyun şu anda açık ('MafiaTheOldCountry.exe').", ConsoleColor.Red);
                Console.WriteLine("Lütfen önce oyunu tamamen kapatıp işlemi yeniden deneyin.");
                return 3;
            }

            Assembly asm = Assembly.GetExecutingAssembly();
            using (Stream resStream = asm.GetManifestResourceStream(ResourceName))
            {
                if (resStream == null)
                {
                    WriteColor("HATA: Gömülü mod paketi (" + ResourceName + ") bu .exe içinde bulunamadı!", ConsoleColor.Red);
                    return 4;
                }

                List<string> installedFiles = new List<string>();
                using (ZipArchive zip = new ZipArchive(resStream, ZipArchiveMode.Read))
                {
                    foreach (ZipArchiveEntry entry in zip.Entries)
                    {
                        if (string.IsNullOrEmpty(entry.Name)) continue; // skip directory entries

                        string relPath = entry.FullName.Replace('/', Path.DirectorySeparatorChar).TrimStart(Path.DirectorySeparatorChar);
                        string destPath = Path.GetFullPath(Path.Combine(gameRoot, relPath));
                        string fullRoot = Path.GetFullPath(gameRoot);
                        if (!destPath.StartsWith(fullRoot, StringComparison.OrdinalIgnoreCase))
                        {
                            WriteColor("Güvenlik hatası: geçersiz arşiv yolu -> " + relPath, ConsoleColor.Red);
                            return 5;
                        }

                        string parent = Path.GetDirectoryName(destPath);
                        if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);

                        using (Stream entryStream = entry.Open())
                        using (FileStream fs = new FileStream(destPath, FileMode.Create, FileAccess.Write, FileShare.None))
                        {
                            entryStream.CopyTo(fs);
                        }
                        installedFiles.Add(relPath);
                    }
                }

                // Write manifest for clean uninstall
                string manifestPath = Path.Combine(gameRoot, ManifestRelPath);
                Directory.CreateDirectory(Path.GetDirectoryName(manifestPath));
                File.WriteAllLines(manifestPath, installedFiles, Encoding.UTF8);

                // Verify core payload files
                string[] required = new[]
                {
                    @"MafiaTheOldCountry\Binaries\Win64\dwmapi.dll",
                    @"MafiaTheOldCountry\Binaries\Win64\xinput1_4.dll",
                    @"MafiaTheOldCountry\Binaries\Win64\hid.dll",
                    @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS.dll",
                    @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\mods.txt",
                    @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\MafiaDualSenseMod\Scripts\main.lua"
                };

                bool allOk = true;
                foreach (string req in required)
                {
                    string fp = Path.Combine(gameRoot, req);
                    if (!File.Exists(fp))
                    {
                        WriteColor("EKSIK DOSYA: " + req, ConsoleColor.Red);
                        allOk = false;
                    }
                }

                if (!allOk)
                {
                    WriteColor("HATA: Kurulum doğrulaması başarısız!", ConsoleColor.Red);
                    return 6;
                }

                WriteColor(string.Format("[BAŞARILI] {0} mod dosyası kuruldu.", installedFiles.Count), ConsoleColor.Green);
            }

            string dsInfo;
            if (IsWiredDualSenseConnected(out dsInfo))
            {
                WriteColor("[OK] Kablolu DualSense algılandı: " + dsInfo, ConsoleColor.Green);
            }
            else
            {
                WriteColor("[BİLGİ] Şu anda kablolu DualSense algılanamadı. Oynamadan önce USB kablo ile bağlayın.", ConsoleColor.Yellow);
            }

            Console.WriteLine();
            WriteColor("ÖNEMLİ HATIRLATMALAR:", ConsoleColor.Cyan);
            Console.WriteLine("  1. Steam -> Mafia: The Old Country -> Özellikler -> Kontrolcü bölümünde");
            Console.WriteLine("     'Steam Girdisini Devre Dışı Bırak' (Disable Steam Input) seçili olmalıdır.");
            Console.WriteLine("  2. Haptic Feedback için DualSense'i USB kabloyla bağlayın ve Windows ses");
            Console.WriteLine("     ayarlarında DualSense hoparlörünün devre dışı olmadığından emin olun.");
            Console.WriteLine("  3. Modu istediğiniz zaman bu .exe ile ([2] Modu Tamamen Kaldır) silebilirsiniz.");
            return 0;
        }

        private static int RunUninstall(string gameRoot)
        {
            Console.WriteLine();
            PrintHeader();
            Console.WriteLine("Mod kaldırılıyor: " + gameRoot);

            if (IsGameRunning())
            {
                WriteColor("HATA: Oyun şu anda açık ('MafiaTheOldCountry.exe').", ConsoleColor.Red);
                Console.WriteLine("Lütfen önce oyunu kapatıp yeniden deneyin.");
                return 3;
            }

            HashSet<string> targets = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string manifestPath = Path.Combine(gameRoot, ManifestRelPath);
            if (File.Exists(manifestPath))
            {
                foreach (string line in File.ReadAllLines(manifestPath, Encoding.UTF8))
                {
                    string t = line.Trim();
                    if (!string.IsNullOrEmpty(t)) targets.Add(t);
                }
            }
            foreach (string fb in FallbackModFiles)
            {
                targets.Add(fb);
            }

            int removedCount = 0;
            foreach (string rel in targets)
            {
                // Never delete the game executable or anything outside Binaries\Win64\
                if (!rel.StartsWith(BinRelDir + @"\", StringComparison.OrdinalIgnoreCase)) continue;
                if (rel.Equals(ExeRelPath, StringComparison.OrdinalIgnoreCase)) continue;

                string full = Path.Combine(gameRoot, rel);
                if (File.Exists(full))
                {
                    try
                    {
                        File.SetAttributes(full, FileAttributes.Normal);
                        File.Delete(full);
                        removedCount++;
                    }
                    catch (Exception ex)
                    {
                        WriteColor("Silinemedi (" + rel + "): " + ex.Message, ConsoleColor.Yellow);
                    }
                }
            }

            // Clean up runtime crashdumps or extra UE4SS artifacts if created by UE4SS inside ue4ss\
            string ue4ssRoot = Path.Combine(gameRoot, BinRelDir, "ue4ss");
            if (Directory.Exists(ue4ssRoot))
            {
                foreach (string extra in new[] { "UE4SS.log", "DualSenseBridge.log", "DualSenseHidProxy.log", "MafiaDualSenseMod.manifest.txt" })
                {
                    string p = Path.Combine(ue4ssRoot, extra);
                    if (File.Exists(p))
                    {
                        try { File.Delete(p); removedCount++; } catch { }
                    }
                }
            }

            foreach (string subRel in CleanupSubDirs)
            {
                string subFull = Path.Combine(gameRoot, subRel);
                if (Directory.Exists(subFull))
                {
                    try
                    {
                        if (Directory.GetFileSystemEntries(subFull).Length == 0)
                        {
                            Directory.Delete(subFull, false);
                        }
                    }
                    catch { }
                }
            }

            if (File.Exists(Path.Combine(gameRoot, @"MafiaTheOldCountry\Binaries\Win64\dwmapi.dll")) ||
                File.Exists(Path.Combine(gameRoot, @"MafiaTheOldCountry\Binaries\Win64\xinput1_4.dll")) ||
                File.Exists(Path.Combine(gameRoot, @"MafiaTheOldCountry\Binaries\Win64\hid.dll")))
            {
                WriteColor("UYARI: Bazı DLL dosyaları kilitli olduğu için silinemedi.", ConsoleColor.Yellow);
                return 7;
            }

            WriteColor(string.Format("[BAŞARILI] Mod tamamen kaldırıldı ({0} mod dosyası temizlendi). Orijinal oyun dosyalarına dokunulmadı.", removedCount), ConsoleColor.Green);
            return 0;
        }

        private static int RunStatus(string gameRoot)
        {
            Console.WriteLine();
            PrintHeader();
            Console.WriteLine("Oyun Klasörü      : " + gameRoot);

            string[] checkPaths = new[]
            {
                @"MafiaTheOldCountry\Binaries\Win64\dwmapi.dll",
                @"MafiaTheOldCountry\Binaries\Win64\xinput1_4.dll",
                @"MafiaTheOldCountry\Binaries\Win64\hid.dll",
                @"MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS.dll",
                @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\mods.txt",
                @"MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\MafiaDualSenseMod\Scripts\main.lua"
            };

            int present = 0;
            foreach (string rel in checkPaths)
            {
                bool exists = File.Exists(Path.Combine(gameRoot, rel));
                if (exists) present++;
                Console.WriteLine("  {0,-68} [{1}]", rel, exists ? "VAR" : "YOK");
            }

            if (present == checkPaths.Length)
                WriteColor("Mod Durumu        : KURULU VE HAZIR", ConsoleColor.Green);
            else if (present == 0)
                WriteColor("Mod Durumu        : KURULU DEĞİL", ConsoleColor.Yellow);
            else
                WriteColor("Mod Durumu        : KISMI KURULUM (Yeniden kurmanız önerilir)", ConsoleColor.Yellow);

            string dsDetail;
            bool dsFound = IsWiredDualSenseConnected(out dsDetail);
            WriteColor("DualSense (USB)   : " + (dsFound ? "BAĞLI (" + dsDetail + ")" : "BAĞLI DEĞİL"),
                dsFound ? ConsoleColor.Green : ConsoleColor.Yellow);

            string bridgeLog = Path.Combine(gameRoot, @"MafiaTheOldCountry\Binaries\Win64\ue4ss\DualSenseBridge.log");
            if (File.Exists(bridgeLog))
            {
                Console.WriteLine("\nSon DualSenseBridge.log özeti:");
                foreach (string l in File.ReadAllLines(bridgeLog))
                {
                    Console.WriteLine("  " + l);
                }
            }
            return 0;
        }

        private static bool IsGameRunning()
        {
            int selfId = Process.GetCurrentProcess().Id;
            foreach (Process p in Process.GetProcesses())
            {
                try
                {
                    if (p.Id == selfId) continue;
                    string name = p.ProcessName;
                    if (name.Equals("MafiaTheOldCountry", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("MafiaTheOldCountry-Win64-Shipping", StringComparison.OrdinalIgnoreCase))
                        return true;
                }
                catch { }
            }
            return false;
        }

        private static string ResolveGameRoot(string explicitDir)
        {
            if (!string.IsNullOrEmpty(explicitDir))
            {
                return ValidateGameDirCandidate(explicitDir);
            }

            // 1. Check current directory and exe directory (+ parents)
            List<string> candidates = new List<string>();
            try
            {
                string exeDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                AddWithParents(candidates, exeDir);
                AddWithParents(candidates, Environment.CurrentDirectory);
            }
            catch { }

            // 2. Query Steam install path and libraryfolders.vdf
            foreach (string steamPath in GetSteamInstallPaths())
            {
                candidates.Add(Path.Combine(steamPath, @"steamapps\common\Mafia The Old Country"));
                string vdf = Path.Combine(steamPath, @"steamapps\libraryfolders.vdf");
                if (File.Exists(vdf))
                {
                    try
                    {
                        string text = File.ReadAllText(vdf);
                        foreach (Match m in Regex.Matches(text, "\"path\"\\s+\"([^\"]+)\""))
                        {
                            string libPath = m.Groups[1].Value.Replace(@"\\", @"\");
                            candidates.Add(Path.Combine(libPath, @"steamapps\common\Mafia The Old Country"));
                        }
                    }
                    catch { }
                }
            }

            // 3. Common defaults
            candidates.Add(@"C:\Program Files (x86)\Steam\steamapps\common\Mafia The Old Country");
            candidates.Add(@"C:\Program Files\Steam\steamapps\common\Mafia The Old Country");

            foreach (string c in candidates)
            {
                string valid = ValidateGameDirCandidate(c);
                if (!string.IsNullOrEmpty(valid)) return valid;
            }
            return null;
        }

        private static void AddWithParents(List<string> list, string dir)
        {
            if (string.IsNullOrEmpty(dir)) return;
            DirectoryInfo di = new DirectoryInfo(dir);
            for (int i = 0; i < 4 && di != null; i++)
            {
                list.Add(di.FullName);
                di = di.Parent;
            }
        }

        private static string ValidateGameDirCandidate(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return null;
            try
            {
                string full = Path.GetFullPath(dir);
                if (File.Exists(Path.Combine(full, ExeRelPath))) return full;
                // In case user gave ...\MafiaTheOldCountry\Binaries\Win64 directly
                if (File.Exists(Path.Combine(full, "MafiaTheOldCountry.exe")))
                {
                    DirectoryInfo d = new DirectoryInfo(full);
                    if (d.Parent != null && d.Parent.Parent != null && d.Parent.Parent.Parent != null)
                    {
                        string up = d.Parent.Parent.Parent.FullName;
                        if (File.Exists(Path.Combine(up, ExeRelPath))) return up;
                    }
                }
            }
            catch { }
            return null;
        }

        private static IEnumerable<string> GetSteamInstallPaths()
        {
            List<string> paths = new List<string>();
            TryReadReg(Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath", paths);
            TryReadReg(Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath", paths);
            TryReadReg(Registry.LocalMachine, @"SOFTWARE\Valve\Steam", "InstallPath", paths);
            return paths;
        }

        private static void TryReadReg(RegistryKey hive, string subKey, string valueName, List<string> outList)
        {
            try
            {
                using (RegistryKey key = hive.OpenSubKey(subKey))
                {
                    object val = key != null ? key.GetValue(valueName) : null;
                    if (val != null)
                    {
                        string s = val.ToString().Replace('/', Path.DirectorySeparatorChar);
                        if (Directory.Exists(s)) outList.Add(s);
                    }
                }
            }
            catch { }
        }

        private static bool IsWiredDualSenseConnected(out string info)
        {
            info = "";
            try
            {
                Guid hidGuid;
                HidD_GetHidGuid(out hidGuid);
                IntPtr devInfo = SetupDiGetClassDevsW(ref hidGuid, null, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
                if (devInfo == IntPtr.Zero || devInfo == INVALID_HANDLE_VALUE) return false;
                try
                {
                    uint index = 0;
                    SP_DEVICE_INTERFACE_DATA ifData = new SP_DEVICE_INTERFACE_DATA();
                    ifData.cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));
                    while (SetupDiEnumDeviceInterfaces(devInfo, IntPtr.Zero, ref hidGuid, index++, ref ifData))
                    {
                        uint reqSize = 0;
                        SetupDiGetDeviceInterfaceDetailW(devInfo, ref ifData, IntPtr.Zero, 0, out reqSize, IntPtr.Zero);
                        if (reqSize == 0) continue;

                        IntPtr detailBuf = Marshal.AllocHGlobal((int)reqSize);
                        try
                        {
                            Marshal.WriteInt32(detailBuf, IntPtr.Size == 8 ? 8 : 6);
                            uint ignoredSize = 0;
                            if (SetupDiGetDeviceInterfaceDetailW(devInfo, ref ifData, detailBuf, reqSize, out ignoredSize, IntPtr.Zero))
                            {
                                string devPath = Marshal.PtrToStringUni(new IntPtr(detailBuf.ToInt64() + 4)) ?? "";
                                string low = devPath.ToLowerInvariant();
                                if (low.Contains("vid_054c") && (low.Contains("pid_0ce6") || low.Contains("pid_0df2")))
                                {
                                    bool isEdge = low.Contains("pid_0df2");
                                    bool isUsb = !low.Contains("bth") && !low.Contains("{00001124");
                                    if (isUsb)
                                    {
                                        info = isEdge ? "DualSense Edge USB (054C:0DF2)" : "DualSense USB (054C:0CE6)";
                                        return true;
                                    }
                                    if (string.IsNullOrEmpty(info))
                                    {
                                        info = "Bluetooth DualSense algılandı (haptics için USB kabloyla bağlayın)";
                                    }
                                }
                            }
                        }
                        finally
                        {
                            Marshal.FreeHGlobal(detailBuf);
                        }
                    }
                }
                finally
                {
                    SetupDiDestroyDeviceInfoList(devInfo);
                }
            }
            catch { }
            return false;
        }

        private static void WriteColor(string text, ConsoleColor color)
        {
            ConsoleColor prev = Console.ForegroundColor;
            Console.ForegroundColor = color;
            Console.WriteLine(text);
            Console.ForegroundColor = prev;
        }
    }
}
