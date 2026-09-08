# PyInstaller build of the OnlyCam desktop application.
# Usage: pyinstaller onlycam.spec

block_cipher = None

analysis = Analysis(
    ["run_onlycam.py"],
    pathex=["."],
    binaries=[],
    datas=[("onlycam/web", "web")],
    hiddenimports=["PIL._tkinter_finder"],
    hookspath=[],
    runtime_hooks=[],
    excludes=["matplotlib", "scipy", "pandas", "tkinter.test"],
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(analysis.pure, analysis.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    analysis.scripts,
    [],
    exclude_binaries=True,
    name="OnlyCam",
    debug=False,
    strip=False,
    upx=False,
    console=False,
)

collection = COLLECT(
    exe,
    analysis.binaries,
    analysis.zipfiles,
    analysis.datas,
    strip=False,
    upx=False,
    name="OnlyCam",
)
