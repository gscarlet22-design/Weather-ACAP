# Web UI preview (development only)

Runs the configuration UI in a desktop browser with no camera.

```
npx -y http-server . -p 8765 -c-1     # from the repo root
# open http://localhost:8765/tools/ui-preview/preview.html
```

- `dev-mock.js` patches `window.fetch` and answers `weather_acap.cgi` with
  canned JSON (`config`, `status`, `ports`, `history`, `cond_history`,
  `preview_overlay`, `device`, `clip_list`, `snapshot_list`, `save`).
- `preview.html` is `app/html/index.html` with one extra `<script src="dev-mock.js">`
  line before `app.js`, and its asset links pointed at `../../app/html/`.
  **When `index.html` changes, regenerate `preview.html`** (copy, then re-add
  the mock line and the `../../app/html/` prefixes) or the preview drifts.

Nothing in this directory is packaged: `acap-build` ships the whole
`app/html/` directory and nothing else, so keep every dev file out of
`app/html/`.
