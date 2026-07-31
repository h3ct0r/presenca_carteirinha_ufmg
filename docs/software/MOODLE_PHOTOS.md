# Exporting Moodle student photos to a tar archive

Step 1 of the avatar pipeline. The `.tar` this produces is fed to the
config-builder, which matches each photo to a student and bundles it into
`config.tar` — see [STUDENT_PHOTOS.md](STUDENT_PHOTOS.md).

By default, Moodle's native export tools only extract text data (like names and emails) and do not support downloading student profile pictures. 

This guide provides a client-side JavaScript workaround. It scrapes the Participants page, downloads the highest resolution available photo for each student, sanitizes their name to be filesystem-friendly, builds a `.tar` binary archive in the browser's memory, and triggers a download.

## Prerequisites

1. You must have appropriate access (Teacher/Professor) to the Moodle course.
2. You must be on the **Participants** page of the target course.
3. **Critical:** You must set the pagination to show all students on a single page. Scroll to the bottom of the list and click **Show all X** (or *Mostrar todos os X*). If you do not do this, the script will only export the visible students.

## Usage Instructions

1. Navigate to your course's **Participants** page and ensure all students are visible on screen.
2. Open your browser's Developer Console:
   - **Windows/Linux:** Press `Ctrl + Shift + J` (Chrome) or `Ctrl + Shift + K` (Firefox).
   - **Mac:** Press `Cmd + Option + J` (Chrome) or `Cmd + Option + K` (Firefox).
3. Copy the script below, paste it into the console, and press **Enter**.
4. A status box will appear in the top-right corner. Wait a few seconds for the script to fetch the images and build the archive.
5. The file `student_photos.tar` will automatically download to your computer.

## The Extraction Script

```javascript
(async () => {
    // Create a temporary UI status box
    let info = document.createElement('div');
    info.style.cssText = 'position:fixed; top:20px; right:20px; background:#222; color:#fff; padding:15px; border-radius:5px; z-index:999999; font-family:sans-serif; box-shadow: 0 4px 6px rgba(0,0,0,0.3);';
    info.textContent = 'Fetching images, please wait...';
    document.body.appendChild(info);

    const images = document.querySelectorAll('.userpicture');
    const files = [];

    for (let img of images) {
        let parentLink = img.closest('a');
        if (!parentLink) continue;
        
        let rawName = parentLink.textContent.trim();
        if (!rawName) continue;
        
        // Clean the name: remove accents, keep only letters/numbers, replace spaces with underscores
        let cleanName = rawName.normalize("NFD").replace(/[\u0300-\u036f]/g, "").replace(/[^a-zA-Z0-9\s]/g, "").trim().replace(/\s+/g, "_");
        
        try {
            // Try to fetch the high-res /f1 version first, fallback to current /f2 thumbnail
            let targetSrc = img.src.replace('/f2', '/f1');
            let res = await fetch(targetSrc);
            if (!res.ok) res = await fetch(img.src);
            
            if (res.ok) {
                let buffer = await res.arrayBuffer();
                files.push({
                    name: cleanName + '.jpg',
                    data: new Uint8Array(buffer)
                });
                info.textContent = `Fetched ${files.length} / ${images.length} images...`;
            }
        } catch (e) {
            console.error("Failed to fetch image for", cleanName);
        }
    }

    info.textContent = 'Generating tar archive...';

    // Manually build the .tar binary structure
    let out = [];
    for (let f of files) {
        let header = new Uint8Array(512);
        
        // File Name (max 99 chars)
        header.set(new TextEncoder().encode(f.name.substring(0, 99)), 0); 
        // File Mode (644), UID, GID
        header.set(new TextEncoder().encode('0000644\0'), 100); 
        header.set(new TextEncoder().encode('0000000\0'), 108); 
        header.set(new TextEncoder().encode('0000000\0'), 116); 
        
        // File Size (octal)
        let sizeStr = f.data.length.toString(8).padStart(11, '0') + '\0';
        header.set(new TextEncoder().encode(sizeStr), 124); 
        
        // Modification Time (octal timestamp)
        let mtimeStr = Math.floor(Date.now() / 1000).toString(8).padStart(11, '0') + '\0';
        header.set(new TextEncoder().encode(mtimeStr), 136); 
        
        // Type flag ('0' for regular file) and ustar magic string
        header[156] = 48; 
        header.set(new TextEncoder().encode('ustar  \0'), 257); 
        
        // Calculate Checksum (requires 8 spaces in the checksum field during calculation)
        header.set(new TextEncoder().encode('        '), 148); 
        let chksum = 0;
        for (let i = 0; i < 512; i++) chksum += header[i];
        let chksumStr = chksum.toString(8).padStart(6, '0') + '\0 ';
        header.set(new TextEncoder().encode(chksumStr), 148);
        
        // Push header and file data
        out.push(header);
        out.push(f.data);
        
        // Pad the end of the file data to a multiple of 512 bytes
        let padding = 512 - (f.data.length % 512);
        if (padding < 512) out.push(new Uint8Array(padding));
    }
    
    // Two empty 512-byte blocks signify the end of a tar archive
    out.push(new Uint8Array(1024)); 
    
    // Bundle into a Blob and trigger the download
    const blob = new Blob(out, {type: 'application/x-tar'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'student_photos.tar';
    a.click();
    
    // Cleanup memory
    URL.revokeObjectURL(url);
    info.textContent = 'Done! Download starting...';
    setTimeout(() => info.remove(), 3000);
})();