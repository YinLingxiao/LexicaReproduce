import {test, expect} from '@playwright/test';

test('unimplemented commands stay visible failures on the real C++ server', async ({page}) => {
	const errors: string[] = [];
	page.on('pageerror', (error) => errors.push(error.message));
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'Review unavailable'})).toBeVisible();
	await expect(page.getByRole('alert')).toContainText('scheduled for the next development phase');
	await expect(page.getByRole('heading', {name: 'surgery'})).toHaveCount(0);
	await page.goto('/stats');
	await expect(page.getByRole('alert')).toContainText('stats_summary');
	await page.getByRole('button', {name: 'Retry'}).click();
	await expect(page.getByRole('alert')).toContainText('stats_summary');
	await page.goto('/library');
	await expect(page.getByRole('alert')).toContainText('library_words');
	await page.goto('/settings');
	await expect(page.getByText('Seed only')).toBeVisible();
	await expect(page.getByText('55', {exact: true}).first()).toBeVisible();
	await page.goto('/?word=zzzzzzzz');
	await expect(page.getByRole('heading', {name: /No entry for “zzzzzzzz”/})).toBeVisible();
	await page.getByRole('link', {name: 'Search', exact: true}).first().click();
	await expect(page.getByText('55 entries · offline')).toBeVisible();
	expect(errors).toEqual([]);
});
