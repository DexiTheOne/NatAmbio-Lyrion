# NatAmbio Alpha for Lyrion

- Verify completed changes and push them to the GitHub repository. Keep unrelated local changes out of the commit.
- For each change that affects the installed plugin, increment the patch version in `plugin/install.xml`, update the helper version and dashboard asset versions to match, and update `RELEASE.md`.
- Push the release commit to `main` and push a matching new `vX.Y.Z` tag. The tag workflow builds and tests the Linux x86-64 plugin and publishes the ZIP and `repo.xml`. Always use a new version for each published package.
- Wait for the release workflow to succeed. Verify that the published `repo.xml` points to the versioned ZIP and that its checksum matches. Report the release version and the steps to install it from Lyrion's Manage Plugins page.
