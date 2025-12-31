-- FluxDM — seeds/defaults.sql
INSERT OR IGNORE INTO categories(name,save_path,extensions)
VALUES
  ('Video',    '~/Videos',    '.mp4,.mkv,.avi,.mov'),
  ('Audio',    '~/Music',     '.mp3,.flac,.wav,.aac'),
  ('Docs',     '~/Documents', '.pdf,.docx,.epub'),
  ('Software', '~/Downloads', '.deb,.rpm,.AppImage,.tar.gz'),
  ('General',  '~/Downloads', NULL);

INSERT OR IGNORE INTO settings(key,value) VALUES
  ('theme',           'dark'),
  ('default_segs',    '8'),
  ('global_limit',    '0'),
  ('progress_ms',     '250');
