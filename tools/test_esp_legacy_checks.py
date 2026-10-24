"""Reject the concrete ROM/console-only CYD false acceptance."""
import unittest
from test_esp_legacy import cyd_application_checks


class CydEvidenceTests(unittest.TestCase):
    """Use actual panel/console geometry, rather than emulator process liveness."""
    def setUp(self):
        self.width, self.height = 320, 284
        self.panel = bytes([128]) * (320 * 240 * 3) + bytes(320 * 44 * 3)
        self.console = bytes(320 * 240 * 3) + bytes([128]) * (320 * 44 * 3)

    def test_rom_and_console_do_not_establish_startup(self):
        row = cyd_application_checks('ets Jul 29 2019\nentry 0x400805b4', self.console, self.width, self.height)
        self.assertFalse(row['application_startup_reached'])
        self.assertFalse(row['lcd_panel_has_content'])
        self.assertGreater(sum(x != 0 for x in self.console), 1000)

    def test_network_begin_precedes_phy_and_is_insufficient(self):
        row = cyd_application_checks('##[BOOT]#\tnetwork.begin', self.panel, self.width, self.height)
        self.assertFalse(row['application_startup_reached'])

    def test_real_application_and_panel_meet_the_checkpoint(self):
        row = cyd_application_checks('##[BOOT]#\tRunning in AP mode', self.panel, self.width, self.height)
        self.assertTrue(all(row.values()))

    def test_console_pixels_cannot_replace_the_panel(self):
        row = cyd_application_checks('##[BOOT]#\tnetserver.begin', self.console, self.width, self.height)
        self.assertTrue(row['application_startup_reached'])
        self.assertFalse(row['lcd_panel_has_content'])

    def test_truncated_raster_is_not_a_frame(self):
        row = cyd_application_checks('Running in AP mode', bytes([128]) * 2000, self.width, self.height)
        self.assertFalse(row['framebuffer_size_valid'])


if __name__ == '__main__':
    unittest.main()
