# cpwrap: like rlwrap, but for copilot suggestions
`cpwrap [arg ...]`  
wraps the execution of whatever command you pass it. It sends the session transcript to [GitHub Copilot](https://github.com/features/copilot) (you must be subscribed) and shows suggestions you can accept by pressing tab, at which point it's just like you had typed that yourself into the command. There is no need for the wrapped program to know anything about copilot; `cpwrap` creates a new pseudo-terminal, so the wrapped program can't (and more importantly doesn't need to) tell the difference between what you typed and what you autocompleted.

Tab=accept, ESC=reject, F1=toggle on/off

https://user-images.githubusercontent.com/6289391/213608944-41bc2ae1-5c50-4c49-b82b-c474c2ee1f55.mp4

# Requirements
* [`node`](https://nodejs.org/)
* copilot `dist` folder somewhere on your system (like what comes with [copilot.vim](https://github.com/github/copilot.vim/tree/release/copilot/dist))

`cpwrap` will look for the dist folder in:
* `CPWRAPPATHDIST` environment variable
* `~/.config/cpwrap/pathdist` file
* `pathcopilotdist` global variable in `cpwrap.c`
* extension locations for vim and neovim

Otherwise, it will prompt for the location and then save that.

# Is this allowed?
I'm not sure; there are some other unofficial plugins. `cpwrap` tries to be a good citizen (identifying itself, giving notifications for show/accept/reject).

# How does it work?
Copilot thinks we are editing a file called `/tmp/shell_session.txt` whose contents are `$ arg ...\n` followed by the transcript of the session.

# Fun fact
My first pass at this used the [Codex API](https://openai.com/blog/openai-codex/), but it ended up being way too expensive. One interesting thing was that when you started a shell session, like `cpwrap sh`, the first suggestion _every time_ was `cat /etc/passwd`. I notice that when you use copilot in that situation, it doesn't say anything.
