"""PS1 DEV CART GUI pages, in sidebar order."""
from pages.run import RunPage
from pages.console import ConsolePage
from pages.memory import MemoryPage
from pages.screen import ScreenPage
from pages.debug import DebugPage
from pages.hotreload import HotReloadPage
from pages.viewers import ViewersPage
from pages.sound import SoundPage
from pages.synth import SynthPage
from pages.card import CardPage
from pages.linktest import LinkTestPage
from pages.video import VideoPage
from pages.audio import AudioPage
from pages.profiler import ProfilerPage
from pages.netplay import NetplayPage
from pages.firmware import FirmwarePage
from pages.settings_page import SettingsPage

PAGES = [RunPage, ConsolePage, MemoryPage, ScreenPage, DebugPage, HotReloadPage, ViewersPage, SoundPage, SynthPage,
         CardPage, LinkTestPage, NetplayPage, VideoPage, AudioPage, ProfilerPage,
         FirmwarePage, SettingsPage]
