# Documentation Usage Guide

## Quick Start

The AltaLux project now includes comprehensive documentation in multiple formats. This guide shows you how to access and use the documentation effectively.

## Documentation Files

### 📚 Main Documentation

1. **README.md** - Project Overview
   - Start here for project introduction
   - Installation and usage instructions
   - Technical overview
   - Use cases and examples
   - Troubleshooting guide

2. **API_REFERENCE.md** - Complete API Reference
   - Detailed class documentation
   - Method signatures and descriptions
   - Code examples for every feature
   - Thread safety guidelines
   - Performance tips

### 💻 Code Documentation

All header files (`.h`) contain inline Doxygen-style documentation:

- **CBaseAltaLuxFilter.h** - Base filter class
- **CAltaLuxFilterFactory.h** - Filter factory
- **CSerialAltaLuxFilter.h** - Serial implementation
- **CParallelSplitLoopAltaLuxFilter.h** - Parallel implementation
- **ScopedBitmapHeader.h** - Memory management helper
- **AltaLux.h** - Plugin interface

## Viewing Documentation

### Option 1: Read Markdown Files (Recommended for Quick Reference)

**Best for**: Quick lookups, code examples, searching specific topics

1. Open any `.md` file in:
   - **VS Code**: Right-click → "Open Preview" (Ctrl+Shift+V)
   - **GitHub**: Automatically rendered
   - **Browser**: Use a Markdown viewer extension

2. **Quick Links**:
   - Questions? → README_DOCUMENTATION.md
   - API details? → API_REFERENCE.md
   - Code usage? → Check header files

### Option 2: Generate HTML Documentation with Doxygen

**Best for**: Complete API documentation, browsing class hierarchy, searchable reference

#### Prerequisites
1. Install Doxygen from https://www.doxygen.nl/download.html
2. Optional: Install Graphviz for diagrams (https://graphviz.org/download/)

#### Generate Documentation

**Windows:**
```cmd
cd E:\Source\GitHub\AltaLux-IrfanView\AltaLux
doxygen Doxyfile
```

**Linux/Mac:**
```bash
cd /path/to/AltaLux
doxygen Doxyfile
```

#### View Generated Documentation

**Windows:**
```cmd
start Documentation\html\index.html
```

**Linux:**
```bash
xdg-open Documentation/html/index.html
```

**Mac:**
```bash
open Documentation/html/index.html
```

### Option 3: Read Comments in Source Code

**Best for**: Understanding implementation while coding

- Open header files in your IDE
- Hover over functions to see documentation tooltips
- VS Code with C++ extension shows inline documentation

## Documentation Structure

### For New Users

**Start Here:**
```
1. README_DOCUMENTATION.md (Project Overview)
   ↓
2. "Usage" section (How to use the plugin)
   ↓
3. "Use Cases" section (Is this right for my image?)
   ↓
4. Try the plugin in IrfanView
```

### For Developers

**Start Here:**
```
1. README_DOCUMENTATION.md (Technical Details section)
   ↓
2. API_REFERENCE.md (Core Classes section)
   ↓
3. CBaseAltaLuxFilter.h (Base class documentation)
   ↓
4. Specific implementation headers
   ↓
5. Start coding with examples
```

### For Contributors

**Start Here:**
```
1. README_DOCUMENTATION.md (Development section)
   ↓
2. DOCUMENTATION_SUMMARY.md (Documentation standards)
   ↓
3. API_REFERENCE.md (Complete API understanding)
   ↓
4. Study implementation in .cpp files
```

## Finding Information

### Common Questions

**"How do I install the plugin?"**
→ README_DOCUMENTATION.md → Installation section

**"How do I use the GUI?"**
→ README_DOCUMENTATION.md → Usage → GUI Mode section

**"What parameters should I use?"**
→ README_DOCUMENTATION.md → Configuration section

**"How do I process an image programmatically?"**
→ API_REFERENCE.md → Complete Usage Example section

**"What does this function do?"**
→ API_REFERENCE.md → Core Classes section
→ Or check the .h file directly

**"Why is processing slow?"**
→ README_DOCUMENTATION.md → Troubleshooting section
→ Or API_REFERENCE.md → Performance Tips section

**"How do I create a custom filter?"**
→ API_REFERENCE.md → Filter Implementations section
→ README_DOCUMENTATION.md → Development section

**"Is this thread-safe?"**
→ API_REFERENCE.md → Thread Safety section

### Search Tips

**In Markdown files (VS Code):**
- Ctrl+F to search current file
- Ctrl+Shift+F to search all files

**In Doxygen HTML:**
- Use the search box in the top-right corner
- Browse the class hierarchy
- Check the "Files" tab for file-level docs
- Use the alphabetical index

**In Source Code:**
- Use your IDE's "Go to Definition" (F12 in VS Code)
- Use "Find All References" (Shift+F12 in VS Code)
- Search for function names across project

## Documentation Maintenance

### Keeping Documentation Updated

When modifying code:

1. **Update header comments** if changing function behavior
2. **Update examples** if changing API
3. **Update README** if adding new features
4. **Regenerate Doxygen** after significant changes

### Regenerating Doxygen

```bash
cd AltaLux
doxygen Doxyfile
```

This updates `Documentation/html/` with latest changes.

## IDE Integration

### Visual Studio Code

1. Install "C/C++" extension
2. Hover over functions to see documentation
3. Ctrl+Space for autocomplete with docs

### Visual Studio

1. XML documentation comments automatically shown
2. Hover over types/functions for quick info
3. Object Browser shows full documentation

### CLion

1. Quick Documentation: Ctrl+Q
2. Parameter Info: Ctrl+P
3. External Documentation: Shift+F1

## Documentation Formats Explained

### Markdown (.md)
- ✅ Easy to read in any text editor
- ✅ Rendered nicely on GitHub
- ✅ Portable and version-controlled
- ❌ No automatic API cross-references

### Doxygen HTML
- ✅ Professional-looking web interface
- ✅ Searchable and cross-referenced
- ✅ Class hierarchy diagrams
- ❌ Requires generation step

### Inline Comments
- ✅ Always up-to-date with code
- ✅ IDE tooltips and IntelliSense
- ✅ Part of source control
- ❌ Requires opening source files

## Best Practices

### When Reading Documentation

1. **Start with Overview** - Understand the big picture first
2. **Check Examples** - Learn by seeing code in action
3. **Verify Version** - Make sure docs match your code version
4. **Cross-reference** - Use multiple doc sources for clarity

### When Writing Code

1. **Read Method Docs** - Understand parameters and return values
2. **Copy Examples** - Start with working code
3. **Check Thread Safety** - Avoid race conditions
4. **Review Performance** - Understand complexity implications

### When Contributing

1. **Document as You Code** - Don't leave it for later
2. **Follow Existing Style** - Match documentation format
3. **Include Examples** - Show how to use new features
4. **Update All Formats** - Header comments AND markdown docs

## Troubleshooting Documentation

### "Doxygen generates warnings"
- Check for broken @see references
- Verify all @param names match function signature
- Look for missing @brief or @details

### "Can't find specific information"
- Try multiple search terms
- Check related classes/functions
- Look in both API_REFERENCE.md and header files

### "Example code doesn't compile"
- Check you have all required includes
- Verify you're using the correct version
- Make sure example is complete (not a snippet)

### "Documentation seems outdated"
- Regenerate Doxygen documentation
- Check Git history for recent changes
- Compare with actual header files

## Quick Reference Card

### File Quick Reference

| Need to...                    | Go to...                           |
|-------------------------------|-------------------------------------|
| Install plugin                | README → Installation               |
| Use GUI                       | README → Usage → GUI Mode           |
| Use in batch                  | README → Usage → Batch Mode         |
| Understand algorithm          | README → Technical Details          |
| See performance               | README → Performance                |
| Get API details               | API_REFERENCE.md                    |
| See code examples             | API_REFERENCE → Usage Example       |
| Understand parallelization    | CParallelSplitLoopAltaLuxFilter.h   |
| Memory management             | ScopedBitmapHeader.h                |
| Plugin interface              | AltaLux.h                           |
| Troubleshoot                  | README → Troubleshooting            |
| Contribute                    | README → Development                |

### Quick Command Reference

```bash
# Generate HTML documentation
doxygen Doxyfile

# View HTML documentation (Windows)
start Documentation\html\index.html

# View HTML documentation (Linux)
xdg-open Documentation/html/index.html

# Search all markdown files (bash)
grep -r "search term" *.md

# Search all header files (bash)
grep -r "function_name" *.h
```

## Getting Help

### If Documentation Doesn't Answer Your Question

1. Check the GitHub Issues page
2. Look at example code in test files
3. Read the source implementation
4. Contact the author via website

### Improving Documentation

Found an error or unclear section?
1. Open an issue on GitHub
2. Submit a pull request with fix
3. Contact the author

## Summary

**Quick Reference**: Markdown files (*.md)  
**Complete API**: Generate Doxygen HTML  
**While Coding**: Read header files (*.h)  
**Learning**: README_DOCUMENTATION.md  
**API Details**: API_REFERENCE.md  

---

**Happy Coding with AltaLux! 🚀**
