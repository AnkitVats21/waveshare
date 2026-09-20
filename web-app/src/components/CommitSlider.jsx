import React, { useState, useEffect } from 'react';

// One reusable range-input pattern used everywhere in the app: live local
// preview while dragging, single network commit on release. Previously this
// exact onChange/onMouseUp/onTouchEnd/onKeyUp boilerplate was copy-pasted in
// 4+ places (volume x2, mic gain, seek, LED speed); now it's one component.
export default function CommitSlider({
  min = 0,
  max = 100,
  step = 1,
  value,
  onCommit,
  disabled = false,
  className = '',
  formatFill = true,
  ...rest
}) {
  const [dragValue, setDragValue] = useState(value);
  const [dragging, setDragging] = useState(false);

  useEffect(() => {
    if (!dragging) setDragValue(value);
  }, [value, dragging]);

  const displayValue = dragging ? dragValue : value;
  const pct = max > min ? ((displayValue - min) / (max - min)) * 100 : 0;

  const commit = () => {
    if (!dragging) return;
    setDragging(false);
    onCommit(dragValue);
  };

  return (
    <input
      type="range"
      min={min}
      max={max}
      step={step}
      value={displayValue}
      disabled={disabled}
      className={`commit-slider ${className}`}
      style={formatFill ? { '--slider-fill': `${pct}%` } : undefined}
      onChange={(e) => {
        setDragging(true);
        setDragValue(Number(e.target.value));
      }}
      onMouseUp={commit}
      onTouchEnd={commit}
      onKeyUp={commit}
      onBlur={commit}
      {...rest}
    />
  );
}
