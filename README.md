# Core RPC agent and services for Windows

- TODO: rewrite `service-policy` (it's C#, too old for recent .NET)

### Environment variables

- `QUBES_INCLUDES` must contain paths containing `windows-utils`, `libvchan` and `qubesdb` includes. Normally it's `<src>/qubes-windows-utils/include;<src>/qubes-core-vchan-xen/windows/include;<src>/qubes-core-qubesdb/include`.
- `QUBES_LIBS` must contain paths containing `windows-utils`, `libvchan` and `qubesdb` libraries. Normally it's `<src>/qubes-windows-utils/bin;<src>/qubes-core-vchan-xen/windows/bin;<src>/qubes-core-qubesdb/windows/bin`.

## Command-line build

`EWDK_PATH` env variable must be set to the root of MS Enterprise WDK for Windows 10/Visual Studio 2022. 

`build.cmd` script builds the solution from command line using the EWDK (no need for external VS installation).

Usage: `build.cmd Release|Debug`
