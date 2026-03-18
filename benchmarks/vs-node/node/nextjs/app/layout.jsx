// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// An App Router project has to have a root layout, and this benchmark has no page worth
// rendering: every route it measures is a Route Handler. This is the smallest thing that
// satisfies the requirement.
export const metadata = {title: "synqt-vs-nextjs"};

export default function RootLayout({children}) {
    return (
        <html lang="en">
            <body>{children}</body>
        </html>
    );
}
