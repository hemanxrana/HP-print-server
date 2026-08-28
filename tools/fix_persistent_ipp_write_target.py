from pathlib import Path
p = Path('experiments/android_print_probe/android_print_probe.ino')
s = p.read_text()
old_rebuilt = '''bool ippUsbWriteAll(const uint8_t *data, size_t n, String &error) {
  size_t offset = 0;
  while (offset < n) {
    const size_t part = min((size_t)USB_CHUNK, n - offset);
    size_t accepted = 0;
    if (!usbHost.ippLiveWrite(data + offset, part, accepted, 30000, error)) return false;
'''
new_rebuilt = '''bool ippUsbWriteAll(const uint8_t *data, size_t n, String &error) {
  size_t offset = 0;
  while (offset < n) {
    const size_t part = min((size_t)USB_CHUNK, n - offset);
    size_t accepted = 0;
    if (!usbHost.ippBulkWrite(data + offset, part, accepted, 30000, error)) return false;
'''
if old_rebuilt not in s:
    raise SystemExit('rebuilt IPP writer target not found')
s = s.replace(old_rebuilt, new_rebuilt, 1)
old_live = '''bool liveIppUsbWrite(const uint8_t *data, size_t length, String &error) {
  size_t offset = 0;
  while (offset < length) {
    const size_t part = min((size_t)USB_CHUNK, length - offset);
    size_t accepted = 0;
    if (!usbHost.ippBulkWrite(data + offset, part, accepted, 30000, error)) return false;
'''
new_live = '''bool liveIppUsbWrite(const uint8_t *data, size_t length, String &error) {
  size_t offset = 0;
  while (offset < length) {
    const size_t part = min((size_t)USB_CHUNK, length - offset);
    size_t accepted = 0;
    if (!usbHost.ippLiveWrite(data + offset, part, accepted, 30000, error)) return false;
'''
if old_live not in s:
    raise SystemExit('live IPP writer target not found')
s = s.replace(old_live, new_live, 1)
p.write_text(s)
print('Restored rebuilt IPP one-shot writer and routed only live mode to persistent writer')
