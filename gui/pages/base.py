"""
PS1 DEV CART GUI: base class of the pages shown next to the sidebar.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import ttkbootstrap as tb


class Page(tb.Frame):
    title = ""          # Sidebar text
    phase = 1           # Pages of later phases are shown as "coming soon"
    # With the original PS1USB firmware: "all" = the page can't be used (covered by a note), "running" = not while a
    # program runs (devcart_note at the top of the page), None = the page works
    needs_devcart = None
    devcart_note = ""

    def __init__(self, master, app):
        super().__init__(master, padding=12)
        self.app = app
        self.action = None      # Name of the action shown in the command preview (pages with several actions)
        self.devcart_label = None
        self.build()

    def show_devcart_note(self, shown):
        """devcart_note under the page's title (its first widget), or hidden."""
        if shown and not self.devcart_label:
            self.devcart_label = tb.Label(self, text=self.devcart_note, bootstyle="warning", wraplength=760, justify="left")
        if self.devcart_label:
            if shown and not self.devcart_label.winfo_manager():
                self.devcart_label.pack(anchor="w", pady=(0, 6), after=self.winfo_children()[0])
            elif not shown:
                self.devcart_label.pack_forget()

    def build(self):
        pass

    def args(self):
        """ps1transfer arguments of the current action (without the program, -n serial and -progress), for the
        command preview. None: nothing to show."""
        return None

    def on_show(self):
        pass

    def on_device_changed(self):
        """Another cartridge was chosen at the top of the window (the page may show the previous one's state)."""
        pass

    def on_devices_listed(self):
        """The cartridges connected were listed again (app.devices)."""
        pass

    def section(self, text, master=None):
        """A titled frame (call track() on it once filled, for pages with several actions)."""
        return tb.Labelframe(master or self, text=text, padding=10)

    def select_action(self, action):
        self.action = action

    def track(self, widget, action):
        """Selects the action when the widget (or a child) gets the focus or a click."""
        for event in ("<FocusIn>", "<Button-1>"):
            widget.bind(event, lambda e, a=action: self.select_action(a), add="+")
        for child in widget.winfo_children():
            self.track(child, action)


class PlaceholderPage(Page):
    """Page of a later phase: what it will do and the ps1transfer parameters to use meanwhile."""
    description = ""
    cli = ""

    def build(self):
        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w")
        tb.Label(self, text="Coming in phase %d of the GUI." % self.phase, bootstyle="warning").pack(anchor="w",
                                                                                                    pady=(4, 12))
        tb.Label(self, text=self.description, wraplength=640, justify="left").pack(anchor="w")
        if self.cli:
            tb.Label(self, text="Meanwhile, with ps1transfer:", font=("", 10, "bold")).pack(anchor="w", pady=(16, 4))
            tb.Label(self, text=self.cli, justify="left", font=("Consolas", 10)).pack(anchor="w")
