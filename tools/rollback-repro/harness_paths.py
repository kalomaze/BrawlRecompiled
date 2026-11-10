"""Location-independent workspace paths supplied by the experiment runner."""
import os
from pathlib import Path
WORKSPACE = Path(os.environ['BRAWL_WORKSPACE_ROOT'])
if not WORKSPACE.is_absolute() or not WORKSPACE.is_dir():
    raise ValueError('BRAWL_WORKSPACE_ROOT must be an existing absolute directory')
RECOMP = WORKSPACE / 'BrawlRecompiled'
TOOLS = Path(__file__).resolve().parent
