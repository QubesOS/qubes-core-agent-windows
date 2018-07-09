<#
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
 #>

# Registry location for path hash -> full path mapping for GetImageRGBA
$RegistryMapPath = 'HKLM:\Software\Invisible Things Lab\Qubes Tools'
$RegistryMapKey = 'AppMap'

$desktopBaseName = $args[0]

$desktopFullName = Get-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $desktopBaseName

Invoke-Item $desktopFullName.$desktopBaseName
