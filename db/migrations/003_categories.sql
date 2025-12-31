-- FluxDM — 003_categories.sql
ALTER TABLE downloads ADD COLUMN category_id INTEGER REFERENCES categories(id);
