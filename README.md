# GR2 Exporter for 3ds Max 2026

A native 3ds Max 2026 plugin that exports meshes to `.gr2` (Granny 3D) format directly through **File > Export**.

Made by **angelstance**.

> **Note:** This plugin exports mesh geometry only. Animation export is not supported.

---

## Installation

1. Download the latest release from the [Releases](../../releases) page.
2. Extract all files into your 3ds Max `stdplugs` folder:
   ```
   C:\Program Files\Autodesk\3ds Max 2026\stdplugs\
   ```
   Files to copy:
   - `gr2exp.dle`
   - `granny2_x64.dll`
3. Restart 3ds Max.
4. Use **File > Export** or **File > Export Selected** and set the file type to **GR2 Exporter**.

---

## Building from Source

### Requirements

- Visual Studio 2022 (v145 toolset)
- [3ds Max 2026 SDK](https://www.autodesk.com/developer-network/platform-technologies/3ds-max)
- Granny 3D SDK (not included — you must obtain this yourself from RAD Game Tools)

### Setup

The project uses two path properties you need to set before building:

**`MaxSDK`** — path to your 3ds Max 2026 SDK `maxsdk` folder.

The 3ds Max SDK installer sets the environment variable `ADSK_3DSMAX_SDK_2026` automatically, so if you installed the SDK normally this is already handled. If not, set it yourself:

```
set ADSK_3DSMAX_SDK_2026=C:\path\to\3ds Max 2026 SDK\maxsdk
```

**`GrannySdkDir`** — path to your Granny SDK root folder (the one containing `include\` and `lib\`):

```
set GrannySdkDir=C:\path\to\grannysdk
```

You can also set these as system environment variables or define them in a `Directory.Build.props` file next to the `.vcxproj`.

### Build

Open `gr2exp.vcxproj` in Visual Studio and build **Release | x64**, or from the command line:

```
MSBuild gr2exp.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output goes to `package\`.
