This project was created entirely from an idea and ChatGPT.

The WORKFLOW.md explains the overall process, but I wanted another spot to explain the workflow.

First, I started a chat and had a few back and forth messages. After about 20 messages and a picture of the CYD, I finally asked for the first firmware.
This chat was iteratively going down a rabbit hole trying to emulate a second game boy and reverse engineering the raw Pokemon protocol.

After about 12 hours of iteration, I took the current code, picture of the board and links to the other projects and started a second chat.
This chat started with the proof that the ESP was capable of the communication. From there it stepped through many tests to slowly implement each component.
Eventually an issue from the first chat was identified, a bit not matching the CRC. This turned out to be an issue from trying to time the read instead of using an interrupt.
This second chat was about 6 hours of iteration.

Once the issue was resolved and all functions tested, a new chat was started that requested ChatGPT to provide comments and begin documentation to upload to GitHub.

Aside from this single file, everything here is AI.
