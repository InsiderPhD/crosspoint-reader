/* TransferQueue (shared/TransferQueue.js.inc): one row per job. Colours come
   from the including page's --border-color / --accent-color variables. */
.tq-list {
  max-height: 320px;
  overflow: auto;
}
.tq-row {
  padding: 8px 0;
  border-top: 1px solid var(--border-color);
  font-size: 0.9rem;
}
.tq-row:first-child { border-top: none; }
.tq-head {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  gap: 12px;
}
.tq-label { min-width: 0; overflow-wrap: anywhere; }
.tq-side { display: flex; align-items: baseline; gap: 8px; flex: none; max-width: 60%; }
.tq-status { text-align: right; opacity: 0.8; }
.tq-row.tq-done .tq-status { color: #27ae60; opacity: 1; }
.tq-row.tq-failed .tq-status { color: #e74c3c; opacity: 1; }
.tq-remove {
  border: none;
  background: none;
  color: inherit;
  opacity: 0.6;
  cursor: pointer;
  font-size: 1rem;
  padding: 0 2px;
  line-height: 1;
}
.tq-remove:hover { opacity: 1; }
.tq-bar {
  height: 6px;
  margin-top: 6px;
  border-radius: 3px;
  background: var(--border-color);
  overflow: hidden;
  position: relative;
}
.tq-fill {
  height: 100%;
  width: 0;
  background: var(--accent-color, #6e9a82);
  transition: width 0.3s ease;
}
/* No byte count known (yet): a sliding stripe instead of a fill. */
.tq-bar.tq-indeterminate .tq-fill {
  width: 30%;
  position: absolute;
  animation: tq-slide 1.2s ease-in-out infinite;
  transition: none;
}
@keyframes tq-slide {
  from { left: -30%; }
  to { left: 100%; }
}
@media (prefers-reduced-motion: reduce) {
  .tq-bar.tq-indeterminate .tq-fill { animation: none; left: 35%; }
}
