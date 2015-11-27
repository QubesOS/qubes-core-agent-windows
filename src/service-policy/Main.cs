/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

using System;
using System.IO;
using System.Linq;
using System.ServiceProcess;
using Microsoft.Win32;

namespace Qubes.CoreAgent
{
    static class ServicePolicy
    {
        // Convert start mode to registry value
        static int Value(this ServiceStartMode mode)
        {
            switch(mode)
            {
                case ServiceStartMode.Automatic:
                    return 2;
                case ServiceStartMode.Manual:
                    return 3;
                case ServiceStartMode.Disabled:
                    return 4;
                default:
                    return -1;
            }
        }

        // Set service's start mode in the registry
        static void SetStartMode(this ServiceController service, ServiceStartMode startMode)
        {
            Console.WriteLine("Setting service '{0}' to '{1}'", service.ServiceName, startMode);
            try
            {
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(string.Format(@"SYSTEM\CurrentControlSet\Services\{0}", service.ServiceName), true))
                {
                    key.SetValue("Start", startMode.Value(), RegistryValueKind.DWord);
                }
            }
            catch (Exception e)
            {
                Console.WriteLine("Exception opening registry key for service '{0}':\n{1}\n{2}", service.ServiceName, e.Message, e.StackTrace);
            }
        }

        static int Main(string[] args)
        {
            string cfgPath = "service-policy.cfg";

            if (args.Length > 0)
                cfgPath = args[0];

            using (var cfg = new StreamReader(cfgPath))
            {
                while (!cfg.EndOfStream)
                {
                    var line = cfg.ReadLine();

                    if (line.Trim().StartsWith("#"))
                        continue;

                    var tokens = line.Split(':');

                    if (tokens.Length < 2)
                        continue;

                    string serviceName = tokens[0].Trim();
                    tokens[1] = tokens[1].Trim();
                    // Capitalize the first letter to make sure it matches enum members.
                    var mode = new string(tokens[1].ToCharArray().Select((c, index) => index == 0 ? char.ToUpperInvariant(c) : char.ToLowerInvariant(c)).ToArray());

                    try
                    {
                        ServiceStartMode startMode = (ServiceStartMode)Enum.Parse(typeof(ServiceStartMode), mode);
                        ServiceController service = new ServiceController(serviceName);
                        service.SetStartMode(startMode);
                    }
                    catch (Exception e)
                    {
                        Console.WriteLine("Exception: {0}\n{1}", e.Message, e.StackTrace);
                        continue;
                    }
                }
            }
            return 0;
        }
    }
}
