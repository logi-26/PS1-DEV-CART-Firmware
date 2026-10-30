"""PS1 DEV CART GUI pages, in sidebar order."""
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
from pages.movie import MoviePage
from pages.card import CardPage
from pages.linktest import LinkTestPage
from pages.profiler import ProfilerPage
from pages.firmware import FirmwarePage
from pages.settings_page import SettingsPage

PAGES = [RunPsxPage, RunYarozePage, ConsolePage, ControllerPage, MemoryPage, ProfilerPage, HotReloadPage, ScreenPage,
         DebugPage, FramePage, ViewersPage, TimPage, SpuPage, SoundPage, SynthPage, MoviePage, CardPage, LinkTestPage,
         CheatsPage, FirmwarePage, SettingsPage]
