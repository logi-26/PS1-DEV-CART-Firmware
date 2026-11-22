"""PS1 DEV CART GUI pages, in sidebar order."""
import os

from pages.run import RunPsxPage, RunYarozePage
from pages.console import ConsolePage
from pages.controller import ControllerPage
from pages.memory import MemoryPage
from pages.cheats import CheatsPage
from pages.screen import ScreenPage
from pages.debug import DebugPage
from pages.frame import FramePage
from pages.hotreload import HotReloadPage
from pages.viewers import ViewersPage
from pages.tim import TimPage
from pages.spu import SpuPage
from pages.sound import SoundPage
from pages.synth import SynthPage
from pages.tracker import TrackerPage
from pages.movie import MoviePage
from pages.card import CardPage
from pages.gamedata import GameDataPage
from pages.linktest import LinkTestPage
from pages.profiler import ProfilerPage
from pages.firmware import FirmwarePage
from pages.settings_page import SettingsPage

PAGES = [RunPsxPage, RunYarozePage, ConsolePage, ControllerPage, MemoryPage, ProfilerPage, HotReloadPage, ScreenPage,
         DebugPage, FramePage, ViewersPage, TimPage, SpuPage, SoundPage, TrackerPage, SynthPage, MoviePage, GameDataPage, CardPage,
         LinkTestPage, CheatsPage, FirmwarePage, SettingsPage]

# Experimental pages, still in development: hidden unless the environment variable PS1DEVCART_EXPERIMENTAL is set
# (e.g. "set PS1DEVCART_EXPERIMENTAL=1" before starting the Manager). Imported only then.
if os.environ.get("PS1DEVCART_EXPERIMENTAL"):
    from pages.netplay import NetPlayPage
    PAGES.insert(PAGES.index(LinkTestPage) + 1, NetPlayPage)
