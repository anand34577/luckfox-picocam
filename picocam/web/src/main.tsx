import { render } from 'preact';
import { App } from './app';
import './styles.css';

render(<App />, document.getElementById('app')!);

// clickable rows, chips and cards are reachable and operable with the keyboard
const KEYED = 'tr.click, .chip, .ev, .feed-item';
const fixKeys = () =>
  document.querySelectorAll<HTMLElement>(KEYED).forEach((el) => {
    if (!el.hasAttribute('tabindex')) {
      el.tabIndex = 0;
      el.setAttribute('role', 'button');
    }
  });
new MutationObserver(fixKeys).observe(document.body, { childList: true, subtree: true });
addEventListener('keydown', (e) => {
  const t = e.target as HTMLElement;
  if ((e.key === 'Enter' || e.key === ' ') && t.matches?.(`${KEYED}, [role=button]`) && t.tagName !== 'BUTTON' && t.tagName !== 'A') {
    e.preventDefault();
    t.click();
  }
});
