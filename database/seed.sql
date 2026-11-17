INSERT INTO events (name, description, venue, starts_at)
SELECT 'Campus Music Night', 'Live music and student performances', 'Main Auditorium', now() + interval '14 days'
WHERE NOT EXISTS (SELECT 1 FROM events WHERE name = 'Campus Music Night');

INSERT INTO seats (event_id, seat_label)
SELECT e.id, s.label
FROM events e
CROSS JOIN (VALUES ('A1'), ('A2'), ('A3'), ('A4'), ('A5'), ('A6'),
                   ('B1'), ('B2'), ('B3'), ('B4')) AS s(label)
WHERE e.name = 'Campus Music Night'
ON CONFLICT (event_id, seat_label) DO NOTHING;

